#include <cstring>
#include <iomanip>
#include <unistd.h> // *_FILENO definitions
#include <linux/limits.h> // PATH_MAX
#include <string>
#include "common.h"
#include "emulator.h"
#include "elf_parser.hpp"
#include "guest.h"

using namespace std;

Emulator::Emulator(vsize_t mem_size, const string& filepath,
                   const vector<string>& argv): mmu(mem_size)
{
	memset(regs, 0, sizeof(regs));
	hi        = 0;
	lo        = 0;
	pc        = 0;
	prev_pc   = 0;
	memset(fpregs, 0, sizeof(fpregs));
	ccs       = 0;
	condition = false;
	jump_addr = 0;
	rec_cov   = false;
	tls       = 0;
	running   = false;
	input     = NULL;
	input_sz  = 0;
	bps_bitmap.resize(mem_size);
	load_elf(filepath, argv);
	Jitter(0x40bd78, mmu, 128*1024, bps_bitmap);
}

vsize_t Emulator::memsize() const {
	return mmu.size();
}

void Emulator::set_reg(uint8_t reg, uint32_t val){
	assert(0 <= reg && reg <= 31);
	if (reg != 0)
		regs[reg] = val;
}

uint32_t Emulator::get_reg(uint8_t reg) const {
	assert(0 <= reg && reg <= 31);
	return (reg ? regs[reg] : 0);
}

void Emulator::sets_reg(uint8_t reg, float val){
	assert(0 <= reg && reg <= 31);
	fpregs[reg] = val;
}

void Emulator::setd_reg(uint8_t reg, double val){
	assert(0 <= reg && reg <= 31 && (reg%2 == 0));
	*(double*)(fpregs+reg) = val;
}

float Emulator::gets_reg(uint8_t reg) const{
	assert(0 <= reg && reg <= 31);
	return fpregs[reg];
}

double Emulator::getd_reg(uint8_t reg) const{
	assert(0 <= reg && reg <= 31 && (reg%2 == 0));
	return *(double*)(fpregs+reg);
}

void Emulator::set_cc(uint8_t cc, bool val){
	assert(0 <= cc && cc <= 7);
	uint8_t bit = 1 << cc;
	ccs = (val ? (ccs | bit) : (ccs & ~bit));
}

bool Emulator::get_cc(uint8_t cc) const {
	assert(0 <= cc && cc <= 7);
	uint8_t bit = 1 << cc;
	return (ccs & bit);
}

void Emulator::set_pc(vaddr_t addr){
	pc = addr;
}

vaddr_t Emulator::get_pc() const {
	return pc;
}

vaddr_t Emulator::get_prev_pc() const {
	return prev_pc;
}

Emulator Emulator::fork() const {
	Emulator new_emu(*this);
	new_emu.mmu = mmu.fork();
	return new_emu;
}

void Emulator::reset(const Emulator& other){
	mmu.reset(other.mmu);
	memcpy(regs, other.regs, sizeof(regs));
	hi         = other.hi;
	lo         = other.lo;
	pc         = other.pc;
	prev_pc    = other.prev_pc;
	memcpy(fpregs, other.fpregs, sizeof(fpregs));
	ccs        = other.ccs;
	condition  = other.condition;
	jump_addr  = other.jump_addr;
	rec_cov    = other.rec_cov;
	tls        = other.tls;
	running    = other.running;
	input      = other.input;
	input_sz   = other.input_sz;
	load_addr  = other.load_addr;
	open_files = other.open_files;
	elfpath    = other.elfpath;
}


void Emulator::load_elf(const string& filepath, const vector<string>& argv){
	// Stack layout described in
	// http://articles.manugarg.com/aboutelfauxiliaryvectors.html
	// I add random numbers at the bottom of the stack for auxv
	cout << "Loading " << filepath << endl;
	Elf_parser elf(filepath);
	mmu.load_elf(elf.get_segments(load_addr));

	// Save absolute file path. Ugly conversions here
	char abspath[PATH_MAX];
	if (!realpath(filepath.c_str(), abspath))
		die("error realpath: %s\n", strerror(errno));
	elfpath.assign(abspath);

	// Set entry
	pc = elf.get_entry();
	prev_pc = pc;
	dbgprintf("Entry 0x%X\n", pc);

	// Allocate the stack
	regs[Reg::sp] = mmu.alloc_stack(256 * 1024);
	dbgprintf("Allocated stack at 0x%X\n", regs[Reg::sp]);

	push_stack(0);

	// Generate random bytes for auxv. Note seed is not initialized
	regs[Reg::sp] -= 16;
	vaddr_t random_bytes = regs[Reg::sp];
	for (int i = 0; i < 16; i++)
		mmu.write<uint8_t>(random_bytes + i, rand());

	// Load ascii args into the stack
	vector<vaddr_t> argv_vm;
	for (const string& arg : argv){
		regs[Reg::sp] -= arg.size()+1;
		mmu.write_mem(regs[Reg::sp], arg.c_str(), arg.size()+1);
		argv_vm.push_back(regs[Reg::sp]);
	}
	argv_vm.push_back(0); // Argv last element must be NULL

	// Align sp
	regs[Reg::sp] = regs[Reg::sp] - 0x3 & ~0x3;

	// Set up auxp. They aren't necessary, I did them tried to solve something
	// unrelated.
	phinfo_t phinfo = elf.get_phinfo();
	Elf32_auxv_t auxv[] = {
		{AT_RANDOM, {random_bytes}},               // Address of 16 random bytes
		{AT_EXECFN, {argv_vm[0]}},                 // Filename of the program
		{AT_PHDR,   {load_addr + phinfo.e_phoff}}, // Pointer to program headers
		{AT_PHENT,  {phinfo.e_phentsize}},         // Size of each entry
		{AT_PHNUM,  {phinfo.e_phnum}},             // Number of entries
		{AT_NULL,   {0}},                          // Auxv end
	};
	// We don't use push_stack or mmu.write because of misalignment checks.
	regs[Reg::sp] -= sizeof(auxv);
	mmu.write_mem(regs[Reg::sp], auxv, sizeof(auxv));

	// Set up envp
	push_stack(0);

	// Set up argv and argc
	for (auto it = argv_vm.rbegin(); it != argv_vm.rend(); ++it)
		push_stack<vaddr_t>(*it);
	push_stack<uint32_t>(argv.size());

	// Set breakpoints. Do it here because we have access to symbols
	set_bps(elf.get_symbols());
}


void Emulator::set_bps(const vector<symbol_t>& symbols){
	set_bp_sym("__libc_malloc", &Emulator::malloc_bp, symbols);
	set_bp_sym("__free", &Emulator::free_bp, symbols);
	set_bp_sym("__libc_realloc", &Emulator::realloc_bp, symbols);
	set_bp_sym("__libc_memalign", &Emulator::memalign_bp, symbols);
	set_bp_sym("__libc_valloc", &Emulator::valloc_bp, symbols);
	set_bp_sym("pvalloc", &Emulator::pvalloc_bp, symbols);
	set_bp_sym("__calloc", &Emulator::calloc_bp, symbols);
	set_bp_sym("memcpy", &Emulator::memcpy_bp, symbols);
}

void Emulator::set_bp_addr(vaddr_t addr, breakpoint_t bp){
	assert(addr < bps_bitmap.size());
	bps[addr] = bp;
	bps_bitmap[addr] = true;
}

void Emulator::set_bp_sym(const string& symbol_name, breakpoint_t bp,
                          const vector<symbol_t>& symbols)
{
	auto it = symbols.begin();
	while (it != symbols.end() && it->symbol_name != symbol_name) ++it;
	if (it == symbols.end())
		die("Not found symbol %s\n", symbol_name.c_str());
	set_bp_addr(it->symbol_value, bp);
	dbgprintf("set breakpoint %s at 0x%lX\n", symbol_name.c_str(), it->symbol_value);
}

void check_repeated_cov_id(uint32_t cov_id, vaddr_t from, vaddr_t to){
	//Expensive, use only for debugging
	static map<pair<vaddr_t, vaddr_t>, atomic_flag> m_branches;
	static map<vaddr_t, atomic_flag> m_hashes;
	static atomic_flag lock_report_repeated = ATOMIC_FLAG_INIT;
	static int rep_hashes = 0;
	auto p = make_pair(from, to);
	if (!m_branches[p].test_and_set()){
		if (m_hashes[cov_id].test_and_set()){
			while (lock_report_repeated.test_and_set());
			rep_hashes++;
			cout << "Repeated hash (" << rep_hashes << "/" << m_branches.size()
			     << "): " << cov_id << ". Collision rate: "
				 << (double)rep_hashes/m_branches.size() << endl;
			lock_report_repeated.clear();
		} else {
			//cout << "new hash: " << cov_id << endl;
		}
	}
}

void add_coverage(cov_t& cov, vaddr_t from, vaddr_t to){
	// Unlike in the JIT, it isn't worth having unique coverage ids here.
	// We would need to store them in a data structure, and check it every time
	// we want to add coverage at runtime:
	/*
		static unordered_map<pair<vaddr_t, vaddr_t>, uint32_t, branch_hasher> cov_ids;
		auto branch = make_pair(from, to);
		if (!cov_ids.count(branch)){
			cov_id = get_new_cov_id(cov.size());
			cov_ids[branch] = cov_id;
		} else
			cov_id = cov_ids[branch];
	*/
	// That's very bad for perf. Instead, just use hash-based coverage ids here.
	// We use the interpreter only for crash reporting anyways
	uint32_t cov_id = branch_hash(from, to) & (cov.size()-1);

	// Mark branch as seen
	cov[cov_id] = 1;

	// Only for debugging
	//check_repeated_cov_id(cov_id, from, to);
}

void Emulator::run_inst(cov_t& cov, Stats& local_stats, bool record_cov){
	// Handle breakpoint. Record coverage if it changed pc, same as we do in
	// branches
	cycle_t cycles = rdtsc2();
	if (bps_bitmap[pc]){
		vaddr_t pc_bf_bp = pc; // pc before breakpoint
		(this->*bps[pc])();
		if (pc != pc_bf_bp)    // pc changed
			if (record_cov) add_coverage(cov, pc_bf_bp, pc);
	}
	local_stats.bp_cycles += rdtsc2() - cycles;

	// Fetch current instruction
	cycles = rdtsc2();
	uint32_t inst   = mmu.read_inst(pc);
	uint8_t  opcode = (inst >> 26) & 0b111111;
	local_stats.fetch_inst_cycles += rdtsc2() - cycles;

	// If needed, take the branch
	// Otherwise, just increment PC so it points to the next instruction
	cycles  = rdtsc2();
	prev_pc = pc;
	if (condition){
		pc        = jump_addr;
		condition = false;
	} else 
		pc += 4;

	// No matter if the branch was taken or not, record coverage
	if (rec_cov){
		if (record_cov) add_coverage(cov, prev_pc, pc);
		rec_cov = false;
	}
	local_stats.jump_cycles += rdtsc2() - cycles;

	// Handle current instruction if it isn't a NOP
	cycles = rdtsc2();
	if (inst)
		(this->*inst_handlers[opcode])(inst);
	local_stats.inst_handl_cycles += rdtsc2() - cycles;
}

void Emulator::run_interpreter(const string& input, cov_t& cov,
                               Stats& local_stats)
{
	// Save provided input. Internal representation is as const char* and not
	// as string so we don't have to perform any copy.
	this->input    = input.c_str();
	this->input_sz = input.size();

	// Perform execution recording number of executed instructions
	uint64_t instr_exec = 0;
	running = true;
	cycle_t cycles = rdtsc1(); // run_cycles
	while (running){
		if (false)
			cout << *this;
		run_inst(cov, local_stats);
		local_stats.instr += 1;

		instr_exec += 1;
		if (instr_exec >= INSTR_TIMEOUT)
			throw RunTimeout();
	}
	local_stats.run_cycles += rdtsc1() - cycles;
}

const char* regs_map[] = {
	"00",  "at", "v0", "v1",
	"a0", "a1", "a2", "a3",
	"t0", "t1", "t2", "t3",
	"t4", "t5", "t6", "t7",
	"s0", "s1", "s2", "s3",
	"s4", "s5", "s6", "s7",
	"t8", "t9", "k0", "k1",
	"gp", "sp", "fp", "ra",
	"hi", "lo"
};
void Emulator::run_jit(const string& input, cov_t& cov, jit_cache_t& jit_cache,
                       Stats& local_stats)
{
	// Save provided input. Internal representation is as const char* and not
	// as string so we don't have to perform any copy.
	this->input    = input.c_str();
	this->input_sz = input.size();

	// JIT block arguments
	vm_state state = {
		regs,
		mmu.get_memory(),
		mmu.get_perms(),
		mmu.get_dirty_vec(),
		mmu.get_pdirty_size(),
		mmu.get_dirty_map(),
		fpregs,
		&ccs,
	};
	exit_info exit_inf;
	uint8_t*  cov_map = cov.data();
	uint32_t  regs_state[2000][35];
	uint64_t  ret;

	// Number of instructions executed in current run
	uint64_t instr_exec = 0;

	running = true;
	jit_block_t jit_block;
	cycle_t cycles;
	while (running){
		// Get the JIT block and run it
		cycles = rdtsc2(); // jit_cache_cycles
		jit_block = jit_cache.cache[pc];
		if (!jit_block){
			// Writing to the cache is thread-safe because it is a vector, but
			// it seems like LLVM jitting is not. Thus, we lock here so there's
			// only one thread jitting at a time.
			// It could happen that two or more threads wait to JIT the same pc.
			// The second will load it from disk cache, so it is not a problem.
			jit_cache.mtx.lock();
			jit_block = Jitter(pc, mmu, cov.size(), bps_bitmap).get_result();
			jit_cache.mtx.unlock();
			jit_cache.cache[pc] = jit_block;
		}
		local_stats.jit_cache_cycles += rdtsc2() - cycles;

		cycles = rdtsc1(); // run_cycles
		ret    = jit_block(&state, &exit_inf, cov_map, regs_state);
		local_stats.instr += ret;
		instr_exec        += ret;
		local_stats.run_cycles += rdtsc1() - cycles;

		// Handle the vm exit
		cycles = rdtsc2(); // vm_exit_cycles
		switch (exit_inf.reason){
			case exit_info::ExitReason::IndirectBranch:
			case exit_info::ExitReason::Call:
				// Just continue compilation
				break;
			case exit_info::ExitReason::Syscall:
				handle_syscall(regs[Reg::v0]);
				break;
			case exit_info::ExitReason::Fault:
				// JIT just tells us there's a fault with no additional info.
				// Run interpreter from last reenter pc and let it throw a more
				// accurate fault. I'm not sure if this is correct as state is
				// not restored and some instructions are executed twice.
				// THINK ABOUT THIS
				throw Fault(Fault::Type::Unknown, -1);

				// We can't do this because it will record coverage with
				// interpreter cov ids
				//run_interpreter(input, cov, local_stats); // this will throw
				die("JIT said fault but interpreter didn't\n");
			case exit_info::ExitReason::Exception:
				die("Exception??\n");
			case exit_info::ExitReason::Rdhwr:
				handle_rdhwr(exit_inf.info1, exit_inf.info2);
				break;
			case exit_info::ExitReason::Breakpoint: {
				// Breakpoint address is reenter_pc - 4
				vaddr_t pc_bf_bp = pc;
				(this->*bps[exit_inf.reenter_pc-4])();
				if (pc != pc_bf_bp){ 
					// Breakpoint handler changed PC. Update reenter_pc and
					// report coverage
					exit_inf.reenter_pc = pc;
					add_coverage(cov, exit_inf.reenter_pc-4, pc);
				}
				break;
			}
			case exit_info::ExitReason::CheckRepCovId: {
				vaddr_t from = exit_inf.info1;
				vaddr_t to   = exit_inf.reenter_pc;
				uint32_t cov_id = exit_inf.info2;
				check_repeated_cov_id(cov_id, from, to);
				break;
			}
			default:
				die("Unknown exit reason: %d\n", exit_inf.reason);
		}
		local_stats.vm_exit_cycles += rdtsc2() - cycles;

		// Check timeout
		if (instr_exec >= INSTR_TIMEOUT)
			throw RunTimeout();

		// DUMP REGS
		if (false){
			for (int h = 0; h < ret; h++){
				cout << hex << setfill('0') << fixed << showpoint;// << setprecision(3);
				cout << "PC:  " << setw(8) << regs_state[h][34] << endl;
				for (int i = 0; i < 34; i++){
					cout << "$" << regs_map[i] << ": " << setw(8) << regs_state[h][i] << "\t";
					if ((i+1)%8 == 0)
						cout << endl;
				}
				cout << endl << endl;
			}
		}
		pc = exit_inf.reenter_pc;
		prev_pc = pc; // not sure about this, we'll see
	}
}

uint64_t Emulator::run_until(vaddr_t pc){
	cov_t dummy1(256*1024);
	Stats dummy3;
	running = true;
	while (this->pc != pc && running){
		run_inst(dummy1, dummy3, false);
		dummy3.instr++;
	}
	if (!running)
		die("Program finished runing while trying to run until 0x%X\n", pc);
	running = false;
	return dummy3.instr;
}

void Emulator::test_bp(){
	cout << *this << endl;
	die("test bp\n");
}

void Emulator::malloc_bp(){
	vsize_t size  = regs[Reg::a0];
	vaddr_t addr  = (size > 0 ? mmu.alloc(size) : 0);
	regs[Reg::v0] = addr;
	prev_pc = pc;
	pc      = regs[Reg::ra];
	dbgprintf("malloc(%u) --> 0x%X\n", size, addr);
}

void Emulator::free_bp(){
	//die("free_bp\n");
	vsize_t addr = regs[Reg::a0];
	prev_pc = pc;
	pc      = regs[Reg::ra];
	dbgprintf("free(0x%X)\n", addr);
}

void Emulator::realloc_bp(){
	die("realloc_bp\n");
}

void Emulator::memalign_bp(){
	die("memalign_bp\n");
}

void Emulator::valloc_bp(){
	die("valloc_bp\n");
}

void Emulator::pvalloc_bp(){
	die("pvalloc_bp\n");
}

void Emulator::calloc_bp(){
	// Compute size and check for overflow
	vsize_t nmemb    = regs[Reg::a0];
	vsize_t size     = regs[Reg::a1];
	vsize_t alloc_sz = nmemb * size;
	if (nmemb != 0 && alloc_sz/nmemb != size)
		die("calloc integer overflow\n");

	// Perform allocation and set memory to zero
	vaddr_t addr = (alloc_sz > 0 ? mmu.alloc(alloc_sz) : 0);
	void* zero = alloca(alloc_sz);
	memset(zero, 0, alloc_sz);
	mmu.write_mem(addr, zero, sizeof(zero));

	regs[Reg::v0] = addr;
	prev_pc = pc;
	pc      = regs[Reg::ra];
	dbgprintf("calloc(%u, %u) --> 0x%X\n", nmemb, size, addr);
}

void Emulator::memcpy_bp(){
	//printf("memcpy_bp\n");
	vaddr_t dst = regs[Reg::a0];
	vaddr_t src = regs[Reg::a1];
	vsize_t len = regs[Reg::a2];
	mmu.copy_mem(dst, src, len);

	regs[Reg::v0] = dst;
	prev_pc = pc;
	pc      = regs[Reg::ra];
	dbgprintf("memcpy(0x%X, 0x%X, %u)\n", dst, src, len);
}

uint32_t Emulator::sys_brk(vaddr_t new_brk, uint32_t& error){
	vaddr_t brk = mmu.get_brk();

	// When new_brk is 0, it's an attempt to get current brk
	if (new_brk){
		error = !mmu.set_brk(new_brk);
		brk   = (error ? brk : new_brk);
	}
	dbgprintf("brk(0x%X) --> 0x%X\n", new_brk, brk);
	return brk;
}

uint32_t Emulator::sys_openat(int32_t dirfd, vaddr_t pathname_addr, int32_t flags,
                              uint32_t& error)
{
	/* cout << *this << endl;
	die(""); */
	string pathname = mmu.read_string(pathname_addr);

	// Result fd
	uint32_t fd;
	
	// Create input file
	if (pathname == "input_file"){
		if ((flags & O_RDWR) == O_RDWR || (flags & O_WRONLY) == O_WRONLY)
			die("opening input file with write permissions");
		
		// Find unused fd
		fd = 3;
		while (open_files.count(fd))
			fd++;

		// God, forgive me for this casting.
		File input_file(flags, (char*)input, input_sz);
		open_files[fd] = move(input_file);
	} else if (pathname == "/dev/tty")
		fd = 1;
	else
		die("Unimplemented openat %s\n", pathname.c_str());

	dbgprintf("openat(%d, \"%s\", %d) --> %d\n", dirfd, pathname.c_str(), flags, fd);
	error = 0;
	return fd;
}

uint32_t Emulator::sys_writev(int32_t fd, vaddr_t iov_addr, int32_t iovcnt,
                              uint32_t& error)
{
	if (fd != STDOUT_FILENO && fd != STDERR_FILENO)
		die("sys_writev trying to write to fd %d\n", fd);

	// Return value
	uint32_t bytes_written = 0;

	// Read iovec structs from guest memory
	guest_iovec iov[iovcnt];
	mmu.read_mem(&iov, iov_addr, iovcnt*sizeof(guest_iovec));

	// For each iovec struct, read its content and print it
	vaddr_t iov_base;
	vsize_t iov_len;
	for (int i = 0; i < iovcnt; i++){
		iov_base = iov[i].iov_base;
		iov_len  = iov[i].iov_len;
		char buf[iov_len + 1];
		mmu.read_mem(buf, iov_base, iov_len);
		buf[iov_len] = 0;
		guestprintf("%s", buf);
		bytes_written += iov_len;
	}

	error = 0;
	return bytes_written;
}

vaddr_t Emulator::sys_mmap2(vaddr_t addr, vsize_t length, uint32_t prot,
                            uint32_t flags, uint32_t fd, uint32_t pgoffset,
                            uint32_t& error)
{
	die("mmap2(0x%X, 0x%X, 0x%X, 0x%X, %d, 0x%X)\n", addr, length, prot,
	    flags, fd, pgoffset);
	return 0;
}

uint32_t Emulator::sys_uname(vaddr_t addr, uint32_t& error){
	guest_uname uname = {
		"Linux",    // sysname
		"Baby emu", // nodename
		"666",      // release
		"666",      // version
		"mips"      // machine
	};
	mmu.write_mem(addr, &uname, sizeof(uname));
	dbgprintf("uname(0x%X) --> 0\n", addr);
	error = 0;
	return 0;
}

uint32_t Emulator::sys_readlink(vaddr_t path_addr, vaddr_t buf_addr,
		                        vsize_t bufsize, uint32_t& error)
{
	uint32_t written = 0;
	string path = mmu.read_string(path_addr);
	if (path == "/proc/self/exe"){
		if (bufsize < elfpath.size())
			die("error readlink\n");
		mmu.write_mem(buf_addr, elfpath.c_str(), elfpath.size());
		written = elfpath.size();
	} else
		die("Unimplemented readlink\n");
	
	dbgprintf("readlink(\"%s\", 0x%X, %d) --> %d (%s)\n", path.c_str(), buf_addr, 
	          bufsize, written, elfpath.c_str());
	error = 0;
	return written;
}

uint32_t Emulator::sys_read(uint32_t fd, vaddr_t buf_addr, vsize_t count,
                            uint32_t& error)
{
	// Stdin, stdout and stderr
	switch (fd){
		case STDIN_FILENO:
			die("reading from stdin\n");
			break;

		case STDOUT_FILENO:
			die("reading from stdout?\n");
			break;

		case STDERR_FILENO:
			die("reading from stderr?\n");
			break;
	}

	// Check if file is open
	if (!open_files.count(fd))
		die("reading from not used fd\n");

	// Check if file is readable
	File& f = open_files[fd];
	if (!f.is_readable())
		die("reading from non readable fd\n");

	// Read
	char* cursor = f.get_cursor();
	vsize_t real_count = f.move_cursor(count);
	mmu.write_mem(buf_addr, cursor, real_count);

	dbgprintf("read(%d, 0x%X, %d) --> %d\n", fd, buf_addr, count, real_count);
	error = 0;
	return real_count;
}

uint32_t Emulator::sys_write(uint32_t fd, vaddr_t buf_addr, vsize_t count,
                             uint32_t& error)
{
	// Stdin, stdout and stderr
	switch (fd){
		case STDIN_FILENO:
			die("writing to stdin?\n");
			break;

		case STDOUT_FILENO:
		case STDERR_FILENO:
			char buf[count + 1];
			mmu.read_mem(buf, buf_addr, count);
			buf[count] = 0;
			guestprintf("%s", buf);
			error = 0;
			return count;
	}

	die("write(%d, 0x%X, %d)\n", fd, buf_addr, count);

	// Check if file is open
	if (!open_files.count(fd))
		die("writing to not used fd\n");

	// Check if file is writable
	File& f = open_files[fd];
	if (!f.is_writable())
		die("writing to non writable fd\n");

	// Write
	char* cursor = f.get_cursor();
	vsize_t real_count = f.move_cursor(count);
	mmu.read_mem(cursor, buf_addr, real_count);

	dbgprintf("write(%d, 0x%X, %d) --> %d\n", fd, buf_addr, count, real_count);
	error = 0;
	return real_count;
}

uint32_t Emulator::sys_fstat64(uint32_t fd, vaddr_t statbuf_addr,
                               uint32_t& error)
{
	struct guest_stat64 s;
	// Stdin, stdout and stderr
	switch (fd){
		case STDIN_FILENO:
			die("fstat stdin?\n");
			break;

		case STDOUT_FILENO:
			guest_stat_stdout(s);
			break;

		case STDERR_FILENO:
			die("fstat stderr?\n");
			break;

		default:
			// Check if file is open
			if (!open_files.count(fd))
				die("fstat64 to not open fd %d\n", fd);

			open_files[fd].stat(s);
	}

	mmu.write_mem(statbuf_addr, &s, sizeof(s));
	dbgprintf("fstat64(%d, 0x%X) --> 0\n", fd, statbuf_addr);
	error = 0;
	return 0;
}

uint32_t Emulator::sys_stat64(vaddr_t pathname_addr, vaddr_t statbuf_addr,
                              uint32_t& error)
{
	string pathname = mmu.read_string(pathname_addr);
	if (pathname == "input_file"){
		struct guest_stat64 s;
		guest_stat_default(s, input_sz);
		mmu.write_mem(statbuf_addr, &s, sizeof(s));

	} else
		die("Unimplemented stat64: %s\n", pathname.c_str());

	dbgprintf("stat64(\"%s\", 0x%X) --> 0\n", pathname.c_str(), statbuf_addr);
	error = 0;
	return 0;
}

uint32_t Emulator::sys_close(uint32_t fd, uint32_t& error){
	if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO)
		goto end;

	// Check if file is open
	if (!open_files.count(fd))
		die("closing not used fd %u\n", fd);
	
	open_files.erase(open_files.find(fd));

end:
	dbgprintf("close(%d)\n", fd);
	error = 0;
	return 0;
}

uint32_t Emulator::sys_llseek(uint32_t fd, uint32_t offset_hi,
                              uint32_t offset_lo, vaddr_t result_addr,
							  uint32_t whence, uint32_t& error)
{
	// Check if file is open
	if (!open_files.count(fd))
		die("llseek to not used fd %u\n", fd);
	File& f = open_files[fd];

	// Perform action
	int64_t offset = (((uint64_t)offset_hi)<<32) | offset_lo;
	switch (whence){
		case SEEK_SET:
			f.set_offset(offset);
			break;

		case SEEK_CUR:
			f.set_offset(f.get_offset() + offset);
			break;

		case SEEK_END:
			f.set_offset(f.get_size() + offset);
			break;
	}

	// Write result
	dbgprintf("llseek(%d, 0x%X, 0x%X, 0x%X, %d) --> %lX\n", fd, offset_hi,
	          offset_lo, result_addr, whence, f.get_offset());
	mmu.write<int64_t>(result_addr, f.get_offset());
	error = 0;
	return 0;
}

uint32_t Emulator::sys_ioctl(uint32_t fd, uint32_t request, vaddr_t argp,
		                     uint32_t& error)
{
	dbgprintf("ioctl(%d, 0x%X, 0x%X)\n", fd, request, argp);
	error = 1;
	return -1;
}

uint32_t Emulator::sys_access(vaddr_t pathname_addr, uint32_t mode, uint32_t& error){
	string pathname = mmu.read_string(pathname_addr);
	dbgprintf("access(%s, %d) --> -1\n", pathname.c_str(), mode);
	error = 1;
	return -1;
}

void Emulator::handle_syscall(uint32_t syscall){
	dbgprintf("syscall %u\n", syscall);
	switch (syscall){
		case 4001: // exit
		case 4246: // exit_group
			running = false;
			dbgprintf("EXIT %d\n", regs[Reg::a0]);
			break;

		case 4003: // read
			regs[Reg::v0] = sys_read(
				regs[Reg::a0],
				regs[Reg::a1],
				regs[Reg::a2],
				regs[Reg::a3]
			);
			break;
		
		case 4004: // write
			regs[Reg::v0] = sys_write(
				regs[Reg::a0],
				regs[Reg::a1],
				regs[Reg::a2],
				regs[Reg::a3]
			);
			break;


		case 4006: // close
			regs[Reg::v0] = sys_close(regs[Reg::a0], regs[Reg::a3]);
			break;

		case 4033: // access
			regs[Reg::v0] = sys_access(regs[Reg::a0], regs[Reg::a1], regs[Reg::a3]);
			break;

		case 4045: // brk
			regs[Reg::v0] = sys_brk(regs[Reg::a0], regs[Reg::a3]);
			break;

		case 4024: // getuid
		case 4047: // getgid
		case 4049: // geteuid
		case 4050: // getegid
			regs[Reg::v0] = 0;
			regs[Reg::a3] = 0;
			break;

		case 4054: // ioctl
			regs[Reg::v0] = sys_ioctl(
				regs[Reg::a0],
				regs[Reg::a1],
				regs[Reg::a2],
				regs[Reg::a3]
			);
			break;

		case 4085: // readlink
			regs[Reg::v0] = sys_readlink(
				regs[Reg::a0],
				regs[Reg::a1],
				regs[Reg::a2],
				regs[Reg::a3]
			);
			break;

		case 4122: // uname
			regs[Reg::v0] = sys_uname(regs[Reg::a0], regs[Reg::a3]);
			break;

		case 4140: // llseek
			regs[Reg::v0] = sys_llseek(
				regs[Reg::a0],
				regs[Reg::a1],
				regs[Reg::a2],
				regs[Reg::a3],
				mmu.read<uint32_t>(regs[Reg::sp]+16),
				regs[Reg::a3]
			);
			break;

		case 4146: // writev
			regs[Reg::v0] = sys_writev(
				regs[Reg::a0],
				regs[Reg::a1],
				regs[Reg::a2],
				regs[Reg::a3]
			);
			break;

		case 4210: // mmap2
			regs[Reg::v0] = sys_mmap2(
				regs[Reg::a0],
				regs[Reg::a1],
				regs[Reg::a2],
				regs[Reg::a3],
				mmu.read<uint32_t>(regs[Reg::sp]+16),
				mmu.read<uint32_t>(regs[Reg::sp]+20),
				regs[Reg::a3]
			);
			break;

		case 4213: // stat64
			regs[Reg::v0] = 
				sys_stat64(regs[Reg::a0], regs[Reg::a1], regs[Reg::a3]);
			break;

		case 4215: // fstat64
			regs[Reg::v0] = 
				sys_fstat64(regs[Reg::a0], regs[Reg::a1], regs[Reg::a3]);
			break;

		case 4283: // set_thread_area
			tls = regs[Reg::a0];
			dbgprintf("set tls --> 0x%X\n", tls);
			regs[Reg::v0] = 0;
			regs[Reg::a3] = 0;
			break;

		case 4288: // openat
			regs[Reg::v0] = sys_openat(
				regs[Reg::a0],
				regs[Reg::a1],
				regs[Reg::a2],
				regs[Reg::a3]
			);
			//die("openat\n");
			break;

		default:
			die("Unimplemented syscall at 0x%X: %d\n", prev_pc, syscall);
	}
}

void Emulator::handle_rdhwr(uint8_t hwr, uint8_t reg){
	uint32_t result = 0;
	switch (hwr){
		case 29: // 0b11101, User Local Register
			if (!tls)
				printf("WARNING reading not set tls\n");
			result = tls;
			break;

		default:
			die("Unimplemented rdhwr at 0x%X: %d\n", prev_pc, hwr);
	}
	
	set_reg(reg, result);
}

ostream& operator<<(ostream& os, const Emulator& emu){
	os << hex << setfill('0') << fixed << showpoint;// << setprecision(3);
	os << "PC:  " << setw(8) << emu.pc << endl;
	for (int i = 0; i < 32; i++){
		os << "$" << regs_map[i] << ": " << setw(8) << emu.regs[i] << "\t";
		if ((i+1)%8 == 0)
			os << endl;
	}
	os << "$hi: " << setw(8) << hi << "\t$lo: " << setw(8) << lo << endl;
	os << endl;
	for (int i = 0; i < 32; i+=2){
		os << "$f" << setw(2) << i << ": " << setw(8) << emu.getd_reg(i) << "\t";
		if ((i+2) % 16 == 0)
			os << endl;
	}

	os << "condition: " << emu.condition << "\t"
	   << "jump addr: " << emu.jump_addr << endl;

	for (const auto& f : emu.open_files)
		cout << "file fd " << f.first << ": " << f.second << endl;
	os << dec;

	return os;
}
