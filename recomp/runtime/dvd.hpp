// The disc, from a folder Dolphin's "Extract Entire Disc" wrote (sys/ and
// files/). Runtime code: no std::format.
#pragma once

#include <filesystem>

namespace qilin::rt {

// Reads sys/fst.bin under `root`. False, with the reason on stderr, if it
// isn't an extracted disc.
bool open_disc(const std::filesystem::path& root);
// The size of the disc's file table (sys/fst.bin), which a console's
// apploader loads to the top of MEM1, below which the arena ends; 0 without
// a disc.
uint32_t disc_fst_size();

}  // namespace qilin::rt
