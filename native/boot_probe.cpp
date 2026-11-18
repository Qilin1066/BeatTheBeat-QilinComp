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
static_assert(DOLRECOMP_ENTRY_POINT == 0x80004050);
constexpr std::uint32_t sdk_init_registers = 0x80004210;
constexpr std::uint32_t sdk_call_init_hardware = 0x80004058;
constexpr std::uint32_t sdk_init_paired_singles = 0x800DBC90;
constexpr std::uint32_t sdk_call_init_fprs = 0x80004374;
constexpr std::uint32_t sdk_init_caches = 0x800DD390;
constexpr std::uint32_t sdk_return_from_init_caches = 0x8000437C;
constexpr std::uint32_t sdk_set_error_handler = 0x800DDF10;
constexpr std::uint32_t sdk_dma_error_handler = 0x800DD240;
constexpr std::uint32_t sdk_machine_check_error = 1;

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

void start_from_loaded_image(CPUState& cpu) {
    // The verified PAL DOL is loaded at its final addresses, with BSS already zeroed.
    // Run the original register setup, then return to __start after its __init_data call.
    cpu.pc = sdk_init_registers;
    cpu.lr = sdk_call_init_hardware;
    func_80004000(&cpu);
    if (cpu.exception || cpu.pc != sdk_init_paired_singles || cpu.lr != sdk_call_init_fprs) {
        throw Stop{"unexpected PAL startup after register setup", cpu.pc,
                   cpu.exception ? cpu.exception : cpu.lr};
    }

    // Native equivalent of the cold-boot __OSPSInit call. The executable code is
    // already linked, so the boot-time instruction cache flash has no work here.
    cpu.hid2 |= PPC_HID2_LSQE | PPC_HID2_PSE;
    ppc_memory_fence();
    std::ranges::fill(cpu.gqr, 0u);
    cpu.gpr[0] = cpu.lr;
    cpu.gpr[3] = 0;
    cpu.pc = cpu.lr;
}

[[noreturn]] void run_probe(CPUState& cpu) {
    // Each native dispatch is cycle-bounded by the emitter. Limit the overall startup probe too.
    constexpr std::size_t max_dispatches = 4'096;
    for (std::size_t count = 0; count < max_dispatches; ++count) {
        if (cpu.pc == sdk_init_caches && cpu.lr == sdk_return_from_init_caches) {
            // The host owns cache coherence. Keep the SDK's DMA error-handler registration
            // by tail-calling the original OSSetErrorHandler with the same arguments.
            ppc_memory_fence();
            cpu.gpr[3] = sdk_machine_check_error;
            cpu.gpr[4] = sdk_dma_error_handler;
            cpu.pc = sdk_set_error_handler;
        }
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
        cpu.msr = PPC_MSR_FP | PPC_MSR_RI | PPC_MSR_IR | PPC_MSR_DR;
        cpu.instruction_fallback = unsupported_instruction;
        cpu.external_read = unmapped_read;
        cpu.external_write = unmapped_write;
        cpu.external_read32 = unmapped_external_read;
        cpu.external_write32 = unmapped_external_write;

        std::cout << "Running locally compiled SOMP01 register setup at 0x"
                  << std::hex << sdk_init_registers << '\n';
        start_from_loaded_image(cpu);
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
