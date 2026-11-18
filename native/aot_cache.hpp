// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/cpu.h"

namespace qilin {

[[noreturn]] void unsupported_cache_address(u32 pc, u32 address);

template<u32 instruction, u32 pc>
inline void lower_generated_operation(CPUState* cpu) {
    constexpr u32 opcode = instruction & 0xFFE007FFu;
    constexpr u32 dcbf = 0x7C0000ACu;
    constexpr u32 dcbst = 0x7C00006Cu;

    if constexpr (opcode == dcbf || opcode == dcbst) {
        constexpr u32 ra = (instruction >> 16) & 31u;
        constexpr u32 rb = (instruction >> 11) & 31u;
        constexpr u32 line_size = 32;
        const u32 address = (ra == 0 ? 0u : cpu->gpr[ra]) + cpu->gpr[rb];
        const u32 line = address & ~(line_size - 1u);

        // Native writes are already visible in the same RAM backing every mapped alias.
        if (!get_ram_ptr(cpu, line, line_size, nullptr)) {
            unsupported_cache_address(pc, address);
        }
        cpu->downcount -= 5;
        cpu->pc = pc + 4;
    } else {
        ppc_fallback_instruction(cpu, instruction, pc);
    }
}

}  // namespace qilin
