// What the GPU must look at again: the console's coherency rule, for the
// renderer's caches. GX reads main memory, not the CPU's data cache, so it
// sees what the CPU wrote only once the game flushes or stores the range
// (DCFlushRange, DCStoreRange, dcbf, dcbst), and what DMA wrote (disc and
// NAND reads). Each such write bumps a generation and stamps the 4 KB pages
// it covers; a texture is re-checked only when a page under it has a newer
// stamp than the one it was validated at. A game that skips the flush gets
// stale data on the console too. Writes come from the running guest thread
// only (natives and lifted code), so nothing here needs atomics.
// Runtime code: no std::format.
#pragma once

#include <cstdint>

namespace qilin::rt {

// A flush, store or DMA write of [addr, addr + length).
void note_guest_write(uint32_t addr, uint32_t length);
// The current generation: everything written so far is at or below it.
uint64_t write_generation();
// The newest generation of a write touching [addr, addr + length) (0 if
// none ever did, or the range is outside MEM1 and MEM2).
uint64_t newest_write(uint32_t addr, uint32_t length);

}  // namespace qilin::rt
