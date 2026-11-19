// The guest machine as the runtime sets it up: the 4 GB guest address space
// with MEM1 and MEM2 committed, and the state start-up leaves behind.
// Runtime code: no std::format.
#pragma once

#include <cstdint>

#include "guest.hpp"

namespace qilin::rt {

inline constexpr uint32_t kMem1 = 0x80000000u, kMem1Bytes = 24u << 20;
inline constexpr uint32_t kMem2 = 0x90000000u, kMem2Bytes = 64u << 20;

// Reserves the guest address space and commits MEM1 and MEM2, zeroed. Sets
// g_mem; false if the host refuses.
bool map_guest_memory();

// Guest memory for the runtime's own objects that the game's code works on
// (GX's FIFO object, VI's thread queue): a block OSInit keeps back from the
// top of the MEM1 arena. Zeroed; never freed. Stops when it runs out.
inline constexpr uint32_t kScratchBytes = 64u << 10;
void set_scratch(uint32_t lo, uint32_t hi);
uint32_t guest_alloc(uint32_t bytes, uint32_t align);

// Thrown by the native code that ends the game (PPCHalt after exit's
// destructors): not an error.
struct Exit {
    int code;
};

}  // namespace qilin::rt
