// The disc, from a folder Dolphin's "Extract Entire Disc" wrote (sys/ and
// files/). Runtime code: no std::format.
#pragma once

#include <filesystem>

namespace qilin::rt {

// Reads sys/fst.bin under `root`. False, with the reason on stderr, if it
// isn't an extracted disc.
bool open_disc(const std::filesystem::path& root);

}  // namespace qilin::rt
