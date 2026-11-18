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

// Thrown by the native code that ends the game (PPCHalt after exit's
// destructors): not an error.
struct Exit {
    int code;
};

}  // namespace qilin::rt
