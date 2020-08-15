#ifndef _EMULATOR_H
#define _EMULATOR_H

#include <unordered_map>
#include "mmu.h"
#include "file.h"
#include "stats.h"
#include "common.h"

/* Idea: memory loaded files appart from input file. 
Map filename -> <pointer, size>
std::unordered_map<std::string, std::pair<char*, size_t>> loaded_files;
*/

class Emulator;
typedef void (Emulator::*const inst_handler_t)(uint32_t);
typedef void (Emulator::*breakpoint_t)();

enum Reg {
	zero, at, v0, v1, a0, a1, a2, a3,
	t0,   t1, t2, t3, t4, t5, t6, t7,
	s0,   s1, s2, s3, s4, s5, s6, s7,
	t8,   t9, k0, k1, gp, sp, fp, ra,
};

struct RunTimeout : std::exception {};


class Emulator {
	private:
		static const uint64_t INSTR_TIMEOUT = 10000000;

		// Memory
		Mmu mmu;

		// Registers
		uint32_t  regs[32];
		uint32_t  hi, lo;
		vaddr_t   pc;
		vaddr_t   prev_pc;

		// FPU registers and condition codes
		float     fpregs[32];
		std::bitset<8> ccs;

		// Handling of branches. If condition is true, pc must be updated to
		// jump_addr in next cycle
		bool      condition;
		vaddr_t   jump_addr;

		// Address of thread local storage, set by emulated program with
		// set_thread_area syscall
		vaddr_t   tls;

		// Is the emulator running?
		bool      running;

		// Current input and size
		const char* input;
		size_t      input_sz;

		// ELF load address
		vaddr_t load_addr;

		// Open files appart from stdin, stdout and stderr
		std::unordered_map<int, File> open_files;

		// Absolute path to current loaded file, used by sys_readlink
		std::string elfpath;

		// Breakpoints
		std::vector<breakpoint_t> breakpoints;        // indexed in `get_bp`
		std::vector<bool>         breakpoints_bitmap; // indexed by address

		// Load elf into memory, allocate stack and set up argv and company
		void load_elf(const std::string& filepath,
		              const std::vector<std::string>& args);

		// Push an element to the stack
		template<class T>
		void push_stack(T val);

		// Breakpoints
		void set_bp(vaddr_t addr, breakpoint_t bp);
		breakpoint_t get_bp(vaddr_t addr) const;

		void test_bp();
		void malloc_bp();   // __libc_malloc
		void free_bp();     // __free
		void realloc_bp();  // __libc_realloc
		void memalign_bp(); // __libc_memalign
		void valloc_bp();   // __libc_valloc
		void pvalloc_bp();  // pvalloc
		void calloc_bp();   // __calloc

		// Syscalls
		uint32_t sys_brk(vaddr_t new_brk, uint32_t& error);
		uint32_t sys_openat(int32_t dirfd, vaddr_t pathname_addr, int32_t flags,
		                    uint32_t& error);
		uint32_t sys_writev(int32_t fd, vaddr_t iov_addr, int32_t iovcnt,
		                    uint32_t& error);
		vaddr_t sys_mmap2(vaddr_t addr, vsize_t length, uint32_t prot,
		                  uint32_t flags, uint32_t fd, uint32_t pgoffset,
		                  uint32_t& error);
		uint32_t sys_uname(vaddr_t addr, uint32_t& error);
		uint32_t sys_readlink(vaddr_t path_addr, vaddr_t buf_addr,
		                      vsize_t bufsize, uint32_t& error);
		uint32_t sys_read(uint32_t fd, vaddr_t buf_addr, vsize_t count,
		                  uint32_t& error);
		uint32_t sys_write(uint32_t fd, vaddr_t buf_addr, vsize_t count,
		                   uint32_t& error);
		uint32_t sys_fstat64(uint32_t fd, vaddr_t statbuf_addr,
		                     uint32_t& error);
		uint32_t sys_stat64(vaddr_t pathname_addr, vaddr_t statbuf_addr,
		                     uint32_t& error);
		uint32_t sys_close(uint32_t fd, uint32_t& error);
		uint32_t sys_llseek(uint32_t fd, uint32_t offset_hi, uint32_t offset_lo,
		                    vaddr_t result_addr, uint32_t whence,
							uint32_t& error);
		uint32_t sys_ioctl(uint32_t fd, uint32_t request, vaddr_t argp,
		                   uint32_t& error);

		void handle_syscall(uint32_t syscall);

		void run_inst(cov_t& cov, Stats& local_stats);


	public:
		Emulator(vsize_t mem_size, const std::string& filepath,
		         const std::vector<std::string>& argv);

		vsize_t memsize() const;

		void set_reg(uint8_t reg, uint32_t val);
		uint32_t get_reg(uint8_t reg) const;

		void sets_reg(uint8_t reg, float val);
		void setd_reg(uint8_t reg, double val);
		float gets_reg(uint8_t reg) const;
		double getd_reg(uint8_t reg) const;

		void set_cc(uint8_t cc, bool val);
		bool get_cc(uint8_t cc) const ;

		void set_pc(vaddr_t addr);
		vaddr_t get_pc() const;
		vaddr_t get_prev_pc() const;

		// Forks the emulator and returns the child
		Emulator fork() const;

		// Resets the emulator to the parent it was previously forked from
		void reset(const Emulator& other);

		// Perform run with provided input. May throw Fault or RunTimeout
		void run(const std::string& input, cov_t& cov, Stats& local_stats);

		// Run emulator until given address, return the number of instructions
		// executed
		uint64_t run_until(vaddr_t pc);

		friend std::ostream& operator<<(std::ostream& os, const Emulator& emu);

	private: // Instruction handlers
		static const inst_handler_t inst_handlers[];
		static const inst_handler_t inst_handlers_R[];
		static const inst_handler_t inst_handlers_RI[];
		static const inst_handler_t inst_handlers_special2[];
		static const inst_handler_t inst_handlers_special3[];
		static const inst_handler_t inst_handlers_COP1[];

		void inst_R(uint32_t);
		void inst_RI(uint32_t);
		void inst_special2(uint32_t);
		void inst_special3(uint32_t);
		void inst_COP1(uint32_t);

		void inst_test(uint32_t);
		void inst_unimplemented(uint32_t);
		void inst_or(uint32_t);
		void inst_bgezal(uint32_t);
		void inst_nop(uint32_t);
		void inst_lui(uint32_t);
		void inst_addiu(uint32_t);
		void inst_lw(uint32_t);
		void inst_and(uint32_t);
		void inst_sw(uint32_t);
		void inst_jalr(uint32_t);
		void inst_beq(uint32_t);
		void inst_addu(uint32_t);
		void inst_sll(uint32_t);
		void inst_bne(uint32_t);
		void inst_jr(uint32_t);
		void inst_lhu(uint32_t);
		void inst_syscall(uint32_t);
		void inst_xor(uint32_t);
		void inst_sltu(uint32_t);
		void inst_sltiu(uint32_t);
		void inst_movn(uint32_t);
		void inst_movz(uint32_t);
		void inst_subu(uint32_t);
		void inst_teq(uint32_t);
		void inst_div(uint32_t);
		void inst_divu(uint32_t);
		void inst_mflo(uint32_t);
		void inst_mul(uint32_t);
		void inst_bltz(uint32_t);
		void inst_blez(uint32_t);
		void inst_rdhwr(uint32_t);
		void inst_bgez(uint32_t);
		void inst_slti(uint32_t);
		void inst_andi(uint32_t);
		void inst_ori(uint32_t);
		void inst_xori(uint32_t);
		void inst_pref(uint32_t);
		void inst_jal(uint32_t);
		void inst_lb(uint32_t);
		void inst_nor(uint32_t);
		void inst_bshfl(uint32_t);
		void inst_seh(uint32_t);
		void inst_seb(uint32_t);
		void inst_wsbh(uint32_t);
		void inst_srl(uint32_t);
		void inst_lh(uint32_t);
		void inst_lbu(uint32_t);
		void inst_lwl(uint32_t);
		void inst_lwr(uint32_t);
		void inst_sb(uint32_t);
		void inst_sh(uint32_t);
		void inst_swl(uint32_t);
		void inst_swr(uint32_t);
		void inst_sllv(uint32_t);
		void inst_srlv(uint32_t);
		void inst_slt(uint32_t);
		void inst_sub(uint32_t);
		void inst_add(uint32_t);
		void inst_j(uint32_t);
		void inst_ll(uint32_t);
		void inst_sc(uint32_t);
		void inst_sync(uint32_t);
		void inst_bgtz(uint32_t);
		void inst_mult(uint32_t);
		void inst_multu(uint32_t);
		void inst_mfhi(uint32_t);
		void inst_mthi(uint32_t);
		void inst_mtlo(uint32_t);
		void inst_ext(uint32_t);
		void inst_sra(uint32_t);
		void inst_clz(uint32_t);
		void inst_lwc1(uint32_t);
		void inst_swc1(uint32_t);
		void inst_ldc1(uint32_t);
		void inst_sdc1(uint32_t);
		void inst_fmt_s(uint32_t);
		void inst_fmt_d(uint32_t);
		void inst_fmt_w(uint32_t);
		void inst_fmt_l(uint32_t);
		void inst_fmt_ps(uint32_t);
		void inst_mfc1(uint32_t);
		void inst_mfhc1(uint32_t);
		void inst_mtc1(uint32_t);
		void inst_mthc1(uint32_t);
		void inst_c_cond_s(uint32_t);
		void inst_c_cond_d(uint32_t);
		void inst_bc1(uint32_t);
		void inst_cfc1(uint32_t);
};

template<class T>
void Emulator::push_stack(T val){
	regs[Reg::sp] -= sizeof(T);
	mmu.write<T>(regs[Reg::sp], val);
}

struct inst_R_t {
	uint8_t s;
	uint8_t t;
	uint8_t d;
	uint8_t S;
	
	inst_R_t(uint32_t inst){
		s = (inst >> 21) & 0b00011111;
		t = (inst >> 16) & 0b00011111;
		d = (inst >> 11) & 0b00011111;
		S = (inst >> 6)  & 0b00011111;
	}
};

struct inst_RI_t {
	uint8_t  s;
	uint16_t C;
	
	inst_RI_t(uint32_t inst){
		s = (inst >> 21) & 0b00011111;
		C = inst & 0xFFFF;
	}
};

struct inst_I_t {
	uint8_t  s;
	uint8_t  t;
	uint16_t C;

	inst_I_t(uint32_t inst){
		s = (inst >> 21) & 0b00011111;
		t = (inst >> 16) & 0b00011111;
		C = inst & 0xFFFF;
	}
};

struct inst_J_t {
	uint32_t A;

	inst_J_t(uint32_t inst){ // 26 bits
		A = inst & 0x3FFFFFF;
	}
};

// Used for FPU
struct inst_F_t {
	uint8_t t;
	uint8_t s;
	uint8_t d;
	uint8_t funct;

	inst_F_t(uint32_t inst){
		t     = (inst >> 16) & 0b00011111;
		s     = (inst >> 11) & 0b00011111;
		d     = (inst >> 6)  & 0b00011111;
		funct = inst & 0b00111111;
	}
};

#endif