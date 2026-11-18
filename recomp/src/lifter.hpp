// Lifter: Broadway code -> C++23, one function per guest function.
//
// Slice 1 covers the integer instruction set with the condition register and
// XER, loads and stores (update, indexed, multiple, string, byte-reversed,
// reservations), branches, calls, tail calls, fall-through into the next
// function, CodeWarrior jump tables and indirect calls through the function
// table. Floating point and paired singles are later slices: a function that
// uses them isn't lifted yet, and the reason is reported. Privileged
// instructions are never lifted (they belong to replaced libraries).
//
// Generated code holds the guest registers it uses in host locals and writes
// them back to the context (rt::Context) only around calls and returns, so
// the host compiler can keep them in registers. Every call checks that the
// callee returned to the instruction after it (a longjmp-style return stops).
#pragma once

#include <cstdint>
#include <string>

#include "cfg.hpp"

namespace qilin {

std::string fn_name(uint32_t addr);  // "fn_80004050"

struct LiftResult {
    bool ok = false;
    std::string code;     // the function's C++ definition
    uint32_t fail_at = 0; // the instruction that stopped it
    std::string why;      // e.g. "floating point (slice 3): fadds"
};

class Lifter {
public:
    explicit Lifter(const Cfg& cfg) : cfg_(cfg) {}
    LiftResult lift(const Function& f) const;

private:
    const Cfg& cfg_;
};

}  // namespace qilin
