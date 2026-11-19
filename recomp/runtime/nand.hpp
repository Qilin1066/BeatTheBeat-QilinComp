// The console's flash (NAND), as a host folder (nand.cpp). Runtime code: no
// std::format.
#pragma once

#include <string_view>

namespace qilin::rt {

// The game's identity on the flash: its disc ID ("SOMP01"), whose first four
// characters make its title's home directory.
void init_nand(std::string_view disc_id);

}  // namespace qilin::rt
