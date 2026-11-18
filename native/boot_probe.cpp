// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <math.h>
#include <string.h>

#include "aot_cache.hpp"
#include "moderngekko/sha256.hpp"

extern "C" {
#include "SOMP01.h"
}

extern "C" void ppc_memory_fence() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

namespace {

constexpr auto dol_size = std::streamoff{3'302'048};
constexpr std::uint32_t entry_point = 0x80004050;
static_assert(DOLRECOMP_ENTRY_POINT == entry_point);

struct Stop {
    std::string_view reason;
    std::uint32_t pc;
    std::uint32_t detail;
};

void unsupported_instruction(CPUState*, u32 instruction, u32 pc) {
    throw Stop{"instruction outside native translation", pc, instruction};
}

u64 unmapped_read(CPUState* cpu, u32 address, u8) {
    throw Stop{"unmapped memory read", cpu->pc, address};
}

void unmapped_write(CPUState* cpu, u32 address, u64, u8) {
    throw Stop{"unmapped memory write", cpu->pc, address};
}

u32 unmapped_external_read(CPUState* cpu, u32 address, u8) {
    throw Stop{"unsupported external read", cpu->pc, address};
}

void unmapped_external_write(CPUState* cpu, u32 address, u32, u8) {
    throw Stop{"unsupported external write", cpu->pc, address};
}

std::vector<std::uint8_t> read_verified_dol(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != dol_size) {
        throw std::runtime_error("Expected the PAL SOMP01 revision 0 main.dol");
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(dol_size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), dol_size) ||
        file.peek() != std::char_traits<char>::eof() ||
        moderngekko::Sha256(bytes) != QILIN_SOMP01_SHA256) {
        throw std::runtime_error("DOL verification failed for PAL SOMP01 revision 0");
    }
    return bytes;
}

void load_sections(const std::vector<std::uint8_t>& dol, std::vector<std::uint8_t>& ram) {
    // A DOL has seven text and eleven data entries in parallel offset, address and size arrays.
    // The RAM starts zeroed, including regions the DOL reserves for uninitialized data.
    for (std::size_t i = 0; i < 18; ++i) {
        const auto offset = read_be32(dol.data() + 4 * i);
        const auto address = read_be32(dol.data() + 0x48 + 4 * i);
        const auto size = read_be32(dol.data() + 0x90 + 4 * i);
        if (size == 0) {
            continue;
        }

        const auto destination = address - GC_RAM_BASE;
        if (offset > dol.size() || size > dol.size() - offset ||
            destination > ram.size() || size > ram.size() - destination) {
            throw std::runtime_error("DOL section is outside the image or native boot memory");
        }
        std::copy_n(dol.begin() + offset, size, ram.begin() + destination);
    }
}

void run_probe(CPUState& cpu) {
    // Each native dispatch is cycle-bounded by the emitter. Limit the overall startup probe too.
    constexpr std::size_t max_dispatches = 4'096;
    for (std::size_t count = 0; count < max_dispatches; ++count) {
        const auto start_pc = cpu.pc;
        cpu.downcount = 0;
        const auto function = (start_pc & 3u) == 0 ? dolrecomp_find_original(start_pc) : nullptr;
        if (!function) {
            throw Stop{"no compiled block", start_pc, 0};
        }
        function(&cpu);
        if (cpu.exception) {
            throw Stop{"guest exception requires a native implementation", cpu.srr0, cpu.exception};
        }
        if (cpu.pc == start_pc && cpu.downcount == 0) {
            throw Stop{"no compiled instruction at address", start_pc, 0};
        }
    }
    throw Stop{"native startup probe reached its dispatch limit", cpu.pc, 0};
}

}  // namespace

[[noreturn]] void qilin::unsupported_cache_address(u32 pc, u32 address) {
    throw Stop{"cache operation outside native RAM", pc, address};
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: qilin-native-boot <main.dol>\n";
        return 2;
    }

    try {
        const auto dol = read_verified_dol(argv[1]);
        std::vector<std::uint8_t> ram(GC_MAIN_RAM_SIZE);
        load_sections(dol, ram);
        CPUState cpu{};
        cpu.ram = ram.data();
        cpu.ram_size = static_cast<u32>(ram.size());
        cpu.pc = entry_point;
        cpu.msr = PPC_MSR_FP | PPC_MSR_RI | PPC_MSR_IR | PPC_MSR_DR;
        cpu.instruction_fallback = unsupported_instruction;
        cpu.external_read = unmapped_read;
        cpu.external_write = unmapped_write;
        cpu.external_read32 = unmapped_external_read;
        cpu.external_write32 = unmapped_external_write;

        std::cout << "Running locally compiled SOMP01 code at 0x" << std::hex << cpu.pc << '\n';
        run_probe(cpu);
    } catch (const Stop& stop) {
        std::cerr << "Native boot stopped at 0x" << std::hex << stop.pc << " (detail 0x"
                  << stop.detail << "): " << stop.reason << '\n';
        return 3;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
