// Hardware-access profile: the loads and stores in each function whose
// address is a constant outside cached MEM1 (hardware registers, the GX
// write-gather pipe, uncached mirrors, MEM2, the locked cache).
//
// Lifted code reaches hardware this way when the SDK's inline functions are
// compiled into game and middleware code: GX commands are stores to the
// write-gather pipe at 0xCC008000, written straight from the caller. The
// runtime has to take those over, so this is part of its to-do list.
//
// Addresses come from constant propagation over each function's GPRs (lis,
// addi, ori, mr, through the control flow; anything else that writes a
// register makes it unknown, and a call the volatile ones, r0 and r3-r12:
// the EABI keeps r1, r2, r13 and r14-r31 across it). Accesses through
// pointers loaded from memory aren't seen: the runtime's checked memory
// stops on those instead.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "cfg.hpp"

namespace qilin {

struct HwAccess {
    uint32_t pc = 0;
    uint32_t ea = 0;    // the address (the first byte)
    uint32_t size = 0;  // bytes; a paired-single access is reported as 8
    bool store = false;
};

// Where an address goes: "" for cached MEM1, else a region or hardware block
// ("GX FIFO (write-gather pipe)", "VI", "MEM2", "unmapped" ...).
std::string_view hw_region(uint32_t ea);

// The constant-address accesses outside cached MEM1 in one function.
std::vector<HwAccess> hardware_accesses(const Cfg& cfg, const Function& f);

}  // namespace qilin
