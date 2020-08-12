#ifndef _MMU_H
#define _MMU_H

#include <stdint.h>
#include <cstdlib>
#include <bitset>
#include <vector>
#include "elf_parser.hpp"

/*
It might be better registering separately dirty memory and dirty perms?
not spending much time reseting anyway

Maybe reduce unnecesary sanity checks

Solve chapuza of typedefs in elf_parser.hpp
*/

// Fault: everything that will end the execution abruptly and will be considered
// a crash
struct Fault : public std::exception {
	enum Type {
		Read,
		Write,
		Exec,
		Uninit,
		OutOfBoundsRead,
		OutOfBoundsWrite,
		OutOfBoundsExec,
		MisalignedRead,
		MisalignedWrite,
		MisalignedExec,
	};

	Fault::Type type;
	vaddr_t     fault_addr;

	Fault(Fault::Type type, vaddr_t fault_addr):
		type(type), fault_addr(fault_addr)
		{}

	friend std::ostream& operator<<(std::ostream& os, const Fault& f);
};


class Mmu {
	public:
		static const uint8_t PERM_READ  = (1 << 0); // Can be read
		static const uint8_t PERM_WRITE = (1 << 1); // Can be written
		static const uint8_t PERM_EXEC  = (1 << 2); // Can be executed
		static const uint8_t PERM_INIT  = (1 << 3); // Has been initialized
		static const vsize_t MAX_BRK_SZ = 0x50000;

	private:
		// Guest virtual memory
		uint8_t* memory;
		vsize_t  memory_len;

		// Byte-level permissions for guest virtual memory
		uint8_t* perms;

		// Next allocation returned by `alloc`
		vaddr_t  next_alloc;

		// Virtual address of the top of the stack region, if allocated
		// Don't confuse with emulator stack pointer
		vaddr_t  stack;

		// Brk is handled as an area after last loaded segment, and before
		// custom allocations with alloc. Its max value is where custom
		// allocations start, and its min value is its initial value
		vaddr_t  brk;
		vaddr_t  min_brk, max_brk;

		// Holds the index of every dirty block (addr/DIRTY_BLOCK_SIZE)
		std::vector<vaddr_t> dirty_blocks;

		// Bitmap for every block, true if dirty
		std::vector<bool>    dirty_bitmap;

		// Checks if range is inside guest memory map, throwing OutOfBounds*
		// fault if not
		void check_bounds(vaddr_t addr, vsize_t len, uint8_t perm);

		// Checks if `addr` is aligned to `align` bytes, throwing Misaligned*
		// fault if not
		void check_alignment(vaddr_t addr, vsize_t align, uint8_t perm);

		// Sets region from `addr` to `addr+len` as dirty
		void set_dirty(vaddr_t addr, vsize_t len);

		// Set permissions `perm` from `addr` to `addr+len`
		// Updates dirty
		void set_perms(vaddr_t addr, vsize_t len, uint8_t perm);

		// Checks that all bytes from `addr` to `addr+len` have `perm`
		// permission. `perm` should be PERM_READ, PERM_WRITE or PERM_EXEC.
		// Throw an exception if permissions are not fulfilled.
		void check_perms(vaddr_t addr, vsize_t len, uint8_t perm);

	public:
		Mmu(vsize_t mem_size = 0);

		Mmu(const Mmu& other);

		Mmu(Mmu&& other) noexcept;

		~Mmu();

		friend void swap(Mmu& first, Mmu& second);

		Mmu& operator=(Mmu other);

		// Get current brk. Attempt to set new brk, performing size checks
		vaddr_t get_brk();
		bool set_brk(vaddr_t new_brk);

		// Allocates a block of `size` bytes. Default perms are RW
		vaddr_t alloc(vsize_t size);

		// Allocates a stack at the end of the guest memory space
		// Returns the bottom of the stack (last valid address plus one)
		vaddr_t alloc_stack(vsize_t size);

		// Read `len` bytes from virtual addr `src` into `dst`.
		// Checks bounds and perms
		void read_mem(void* dst, vaddr_t src, vsize_t len);

		// Write `len` bytes from `src` into virtual addr `dst`.
		// Checks bounds and perms
		void write_mem(vaddr_t dst, const void* src, vsize_t len);

		// Read and return and instruction from virtual addr `dst`.
		// Checks bounds, perms and alignment
		uint32_t read_inst(vaddr_t addr);

		// Reads a value from memory. Checks bounds, perms and alignment
		template <class T>
		T read(vaddr_t addr);

		// Writes a value to memory. Checks bounds, perms and alignment
		template <class T>
		void write(vaddr_t addr, T value);

		// Read a string from memory. Checks bounds, perms and alignment
		std::string read_string(vaddr_t addr);

		// Forks the mmu and returns the child
		Mmu fork();

		// Resets the mmu to the parent it was previously forked from
		void reset(const Mmu& other);

		// Load elf segments into memory and update `next_alloc` beyond them
		void load_elf(const std::vector<segment_t>& segments);

		// Hexdumps the memory
		friend std::ostream& operator<<(std::ostream& os, const Mmu& mmu);
};

template<class T>
T Mmu::read(vaddr_t addr){
	T result;
	read_mem(&result, addr, sizeof(T));
	// Perfom this check the last as it is the least important
	check_alignment(addr, sizeof(T), PERM_READ);
	return result;
}

template<class T>
void Mmu::write(vaddr_t addr, T value){
	write_mem(addr, &value, sizeof(T));
	// Perfom this check the last as it is the least important
	check_alignment(addr, sizeof(T), PERM_WRITE);
}

#endif