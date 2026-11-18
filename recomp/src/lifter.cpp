#include "lifter.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <set>

namespace qilin {

using ppc::Insn;
using ppc::Op;

std::string fn_name(uint32_t addr) { return std::format("fn_{:08X}", addr); }

namespace {

std::string R(unsigned n) { return std::format("r{}", n); }
std::string RA0(unsigned n) { return n ? R(n) : std::string("0u"); }
std::string hex(uint32_t v) { return std::format("0x{:X}u", v); }
std::string label(uint32_t a) { return std::format("L_{:08X}", a); }

// PowerPC rotate mask: bits mb..me (IBM numbering), wrapping if mb > me.
uint32_t rot_mask(unsigned mb, unsigned me) {
    const uint32_t m = (0xFFFFFFFFu >> mb) ^ (me >= 31 ? 0u : 0xFFFFFFFFu >> (me + 1));
    return mb <= me ? m : ~m;
}

struct Unsupported {
    std::string why;
};

class Emitter {
public:
    Emitter(const Cfg& cfg, const Function& f) : cfg_(cfg), f_(f) {}

    LiftResult run();

private:
    void line(const std::string& s) { body_ += "    " + s + "\n"; }
    void instruction(uint32_t pc, const Insn& in);
    std::string ea_d(const Insn& in) const;  // (rA|0) + d
    std::string ea_x(const Insn& in) const;  // (rA|0) + rB
    std::string branch_cond(const Insn& in, uint32_t pc);
    std::string call_direct(uint32_t pc, uint32_t target);
    std::string call_indirect(uint32_t pc, const std::string& target);
    std::string tail_direct(uint32_t target);
    std::string ret();
    void record(const Insn& in, const std::string& rd);
    void use(unsigned r) { gprs_.insert(r); }

    const Cfg& cfg_;
    const Function& f_;
    std::string body_;
    std::set<unsigned> gprs_;
    std::set<uint32_t> targets_;  // labels that something jumps to
    bool out_ = false, in_ = false;
};

std::string Emitter::ea_d(const Insn& in) const {
    const uint32_t d = static_cast<uint32_t>(in.simm());
    return in.ra() ? std::format("r{} + {}", in.ra(), hex(d)) : hex(d);
}

std::string Emitter::ea_x(const Insn& in) const {
    return in.ra() ? std::format("r{} + r{}", in.ra(), in.rb()) : R(in.rb());
}

std::string Emitter::ret() {
    out_ = true;
    return "out(); c.ret = lr; return;";
}

std::string Emitter::call_direct(uint32_t pc, uint32_t target) {
    out_ = in_ = true;
    return std::format("lr = {0}; out(); {1}(c); in(); if (c.ret != {0}) rt::bad_return({2}, c.ret);", hex(pc + 4),
                       fn_name(target), hex(pc));
}

std::string Emitter::call_indirect(uint32_t pc, const std::string& target) {
    out_ = in_ = true;
    return std::format("out(); rt::call(c, {1}, {2}); in(); if (c.ret != {0}) rt::bad_return({2}, c.ret);", hex(pc + 4),
                       target, hex(pc));
}

std::string Emitter::tail_direct(uint32_t target) {
    out_ = true;
    return std::format("out(); {}(c); return;", fn_name(target));
}

// The condition of a bc/bclr/bcctr; "" if it always branches. Emits the CTR
// decrement first when BO asks for it.
std::string Emitter::branch_cond(const Insn& in, uint32_t pc) {
    const unsigned bo = in.rd(), bi = in.ra();
    std::string cond;
    if (!(bo & 4)) {
        line("--ctr;");
        cond = (bo & 2) ? "ctr == 0" : "ctr != 0";
    }
    if (!(bo & 0x10)) {
        const std::string bit = std::format("rt::crb(cr, {})", bi);
        const std::string test = (bo & 8) ? bit : "!" + bit;
        cond = cond.empty() ? test : cond + " && " + test;
    }
    (void)pc;
    return cond;
}

// The record form's CR0 update. Only instructions with a record bit have one:
// in ori, xori and the like, bit 31 is part of the immediate.
void Emitter::record(const Insn& in, const std::string& rd) {
    if ((ppc::spec(in.op).flags & ppc::kRc) && in.rc()) line(std::format("cr = rt::set_cr0(cr, {}, xer);", rd));
}

void Emitter::instruction(uint32_t pc, const Insn& in) {
    const ppc::Spec& s = ppc::spec(in.op);
    if (s.flags & ppc::kPrivileged) throw Unsupported{std::format("privileged: {}", s.name)};
    if (s.flags & ppc::kPaired) throw Unsupported{std::format("paired single (slice 4): {}", s.name)};
    if (s.flags & ppc::kFloat) throw Unsupported{std::format("floating point (slice 3): {}", s.name)};

    const unsigned rd = in.rd(), ra = in.ra(), rb = in.rb();
    const std::string D = R(rd), A = R(ra), B = R(rb), S = R(rd);
    const std::string imm = hex(static_cast<uint32_t>(in.simm()));
    const std::string uimm = hex(in.uimm());

    // XO-form arithmetic through rt::add3: result, carry, overflow.
    auto add3 = [&](const std::string& a, const std::string& b, const std::string& c, bool sets_ca) {
        line(std::format("{{ const auto s = rt::add3({}, {}, {}); {} = s.v;{}{} }}", a, b, c, D,
                         sets_ca ? " xer = rt::set_ca(xer, s.carry);" : "",
                         (s.flags & ppc::kOe) && in.oe() ? " xer = rt::set_ov(xer, s.ov);" : ""));
        use(rd);
        record(in, D);
    };
    auto logical = [&](const std::string& expr) {
        use(ra), use(rd);
        line(std::format("{} = {};", A, expr));
        record(in, A);
    };
    auto load = [&](const std::string& fn, const std::string& ea, bool update) {
        use(rd);
        if (ra) use(ra);
        line(std::format("{{ const uint32_t ea = {}; QILIN_PC({}); {} = rt::{}(ea);{} }}", ea, hex(pc), D, fn,
                         update ? std::format(" {} = ea;", A) : ""));
    };
    auto store = [&](const std::string& fn, const std::string& ea, bool update) {
        use(rd);
        if (ra) use(ra);
        line(std::format("{{ const uint32_t ea = {}; QILIN_PC({}); rt::{}(ea, {});{} }}", ea, hex(pc), fn, S,
                         update ? std::format(" {} = ea;", A) : ""));
    };
    auto crop = [&](const std::string& expr) {  // crb(cr,A) and crb(cr,B) as a, b
        line(std::format("{{ const uint32_t a = rt::crb(cr, {}), b = rt::crb(cr, {}); cr = rt::set_crb(cr, {}, {}); }}",
                         ra, rb, rd, expr));
    };
    if (ra && (in.op == Op::addi || in.op == Op::addis)) use(ra);

    switch (in.op) {
    // ---- arithmetic ----
    case Op::addi:  // li when rA = 0
        use(rd);
        line(ra ? std::format("{} = {} + {};", D, A, imm) : std::format("{} = {};", D, imm));
        break;
    case Op::addis:  // lis when rA = 0
        use(rd);
        line(ra ? std::format("{} = {} + {};", D, A, hex(in.uimm() << 16)) : std::format("{} = {};", D, hex(in.uimm() << 16)));
        break;
    case Op::add: use(ra), use(rb); add3(A, B, "0", false); break;
    case Op::addc: use(ra), use(rb); add3(A, B, "0", true); break;
    case Op::adde: use(ra), use(rb); add3(A, B, "rt::ca(xer)", true); break;
    case Op::addme: use(ra); add3(A, "0xFFFFFFFFu", "rt::ca(xer)", true); break;
    case Op::addze: use(ra); add3(A, "0u", "rt::ca(xer)", true); break;
    case Op::subf: use(ra), use(rb); add3("~" + A, B, "1", false); break;
    case Op::subfc: use(ra), use(rb); add3("~" + A, B, "1", true); break;
    case Op::subfe: use(ra), use(rb); add3("~" + A, B, "rt::ca(xer)", true); break;
    case Op::subfme: use(ra); add3("~" + A, "0xFFFFFFFFu", "rt::ca(xer)", true); break;
    case Op::subfze: use(ra); add3("~" + A, "0u", "rt::ca(xer)", true); break;
    case Op::neg: use(ra); add3("~" + A, "0u", "1", false); break;
    case Op::addic:
    case Op::addic_rc:
        use(ra), use(rd);
        line(std::format("{{ const auto s = rt::add3({}, {}, 0); {} = s.v; xer = rt::set_ca(xer, s.carry); }}", A, imm, D));
        if (in.op == Op::addic_rc) line(std::format("cr = rt::set_cr0(cr, {}, xer);", D));
        break;
    case Op::subfic:
        use(ra), use(rd);
        line(std::format("{{ const auto s = rt::add3(~{}, {}, 1); {} = s.v; xer = rt::set_ca(xer, s.carry); }}", A, imm, D));
        break;
    case Op::mulli: use(ra), use(rd); line(std::format("{} = {} * {};", D, A, imm)); break;
    case Op::mullw:
        use(ra), use(rb), use(rd);
        line(std::format("{{ const uint32_t a = {}, b = {}; {} = a * b;{} }}", A, B, D,
                         (s.flags & ppc::kOe) && in.oe() ? " xer = rt::set_ov(xer, rt::mullw_ov(a, b));" : ""));
        record(in, D);
        break;
    case Op::mulhw: use(ra), use(rb), use(rd); line(std::format("{} = rt::mulhw({}, {});", D, A, B)); record(in, D); break;
    case Op::mulhwu: use(ra), use(rb), use(rd); line(std::format("{} = rt::mulhwu({}, {});", D, A, B)); record(in, D); break;
    case Op::divw:
    case Op::divwu: {
        use(ra), use(rb), use(rd);
        const bool sgn = in.op == Op::divw;
        line(std::format("{{ const uint32_t a = {}, b = {}; {} = rt::{}(a, b);{} }}", A, B, D, sgn ? "divw" : "divwu",
                         (s.flags & ppc::kOe) && in.oe()
                             ? std::format(" xer = rt::set_ov(xer, {});", sgn ? "rt::divw_ov(a, b)" : "b == 0")
                             : ""));
        record(in, D);
        break;
    }
    // ---- compare ----
    case Op::cmp: use(ra), use(rb); line(std::format("cr = rt::set_crf(cr, {}, rt::cmps({}, {}, xer));", in.crfd(), A, B)); break;
    case Op::cmpl: use(ra), use(rb); line(std::format("cr = rt::set_crf(cr, {}, rt::cmpu({}, {}, xer));", in.crfd(), A, B)); break;
    case Op::cmpi: use(ra); line(std::format("cr = rt::set_crf(cr, {}, rt::cmps({}, {}, xer));", in.crfd(), A, imm)); break;
    case Op::cmpli: use(ra); line(std::format("cr = rt::set_crf(cr, {}, rt::cmpu({}, {}, xer));", in.crfd(), A, uimm)); break;
    // ---- logical ----
    case Op::and_: use(rb); logical(std::format("{} & {}", S, B)); break;
    case Op::andc: use(rb); logical(std::format("{} & ~{}", S, B)); break;
    case Op::or_: use(rb); logical(std::format("{} | {}", S, B)); break;
    case Op::orc: use(rb); logical(std::format("{} | ~{}", S, B)); break;
    case Op::nor: use(rb); logical(std::format("~({} | {})", S, B)); break;
    case Op::nand: use(rb); logical(std::format("~({} & {})", S, B)); break;
    case Op::eqv: use(rb); logical(std::format("~({} ^ {})", S, B)); break;
    case Op::xor_: use(rb); logical(std::format("{} ^ {}", S, B)); break;
    case Op::andi_rc: use(ra), use(rd); line(std::format("{} = {} & {}; cr = rt::set_cr0(cr, {}, xer);", A, S, uimm, A)); break;
    case Op::andis_rc:
        use(ra), use(rd);
        line(std::format("{} = {} & {}; cr = rt::set_cr0(cr, {}, xer);", A, S, hex(in.uimm() << 16), A));
        break;
    case Op::ori: logical(std::format("{} | {}", S, uimm)); break;
    case Op::oris: logical(std::format("{} | {}", S, hex(in.uimm() << 16))); break;
    case Op::xori: logical(std::format("{} ^ {}", S, uimm)); break;
    case Op::xoris: logical(std::format("{} ^ {}", S, hex(in.uimm() << 16))); break;
    case Op::cntlzw: logical(std::format("static_cast<uint32_t>(std::countl_zero({}))", S)); break;
    case Op::extsb: logical(std::format("static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>({} & 0xFFu)))", S)); break;
    case Op::extsh:
        logical(std::format("static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>({} & 0xFFFFu)))", S));
        break;
    // ---- rotate and shift ----
    case Op::rlwinm: {
        const uint32_t m = rot_mask(in.mb(), in.me());
        const std::string rot = rb ? std::format("std::rotl({}, {})", S, rb) : S;
        logical(std::format("{} & {}", rot, hex(m)));
        break;
    }
    case Op::rlwnm:
        use(rb);
        logical(std::format("std::rotl({}, static_cast<int>({} & 31u)) & {}", S, B, hex(rot_mask(in.mb(), in.me()))));
        break;
    case Op::rlwimi: {
        const uint32_t m = rot_mask(in.mb(), in.me());
        const std::string rot = rb ? std::format("std::rotl({}, {})", S, rb) : S;
        logical(std::format("({} & {}) | ({} & {})", rot, hex(m), A, hex(~m)));
        break;
    }
    case Op::slw: use(rb); logical(std::format("rt::slw({}, {})", S, B)); break;
    case Op::srw: use(rb); logical(std::format("rt::srw({}, {})", S, B)); break;
    case Op::sraw:
    case Op::srawi: {
        use(ra), use(rd);
        const std::string n = in.op == Op::sraw ? (use(rb), B) : hex(rb);
        line(std::format("{{ const auto s = rt::sraw({}, {}); {} = s.v; xer = rt::set_ca(xer, s.carry); }}", S, n, A));
        record(in, A);
        break;
    }
    // ---- loads and stores ----
    case Op::lbz: load("ld8", ea_d(in), false); break;
    case Op::lbzu: load("ld8", ea_d(in), true); break;
    case Op::lbzx: use(rb); load("ld8", ea_x(in), false); break;
    case Op::lbzux: use(rb); load("ld8", ea_x(in), true); break;
    case Op::lhz: load("ld16", ea_d(in), false); break;
    case Op::lhzu: load("ld16", ea_d(in), true); break;
    case Op::lhzx: use(rb); load("ld16", ea_x(in), false); break;
    case Op::lhzux: use(rb); load("ld16", ea_x(in), true); break;
    case Op::lha: load("lha", ea_d(in), false); break;
    case Op::lhau: load("lha", ea_d(in), true); break;
    case Op::lhax: use(rb); load("lha", ea_x(in), false); break;
    case Op::lhaux: use(rb); load("lha", ea_x(in), true); break;
    case Op::lwz: load("ld32", ea_d(in), false); break;
    case Op::lwzu: load("ld32", ea_d(in), true); break;
    case Op::lwzx: use(rb); load("ld32", ea_x(in), false); break;
    case Op::lwzux: use(rb); load("ld32", ea_x(in), true); break;
    case Op::lhbrx: use(rb); load("ld16r", ea_x(in), false); break;
    case Op::lwbrx: use(rb); load("ld32r", ea_x(in), false); break;
    case Op::stb: store("st8", ea_d(in), false); break;
    case Op::stbu: store("st8", ea_d(in), true); break;
    case Op::stbx: use(rb); store("st8", ea_x(in), false); break;
    case Op::stbux: use(rb); store("st8", ea_x(in), true); break;
    case Op::sth: store("st16", ea_d(in), false); break;
    case Op::sthu: store("st16", ea_d(in), true); break;
    case Op::sthx: use(rb); store("st16", ea_x(in), false); break;
    case Op::sthux: use(rb); store("st16", ea_x(in), true); break;
    case Op::stw: store("st32", ea_d(in), false); break;
    case Op::stwu: store("st32", ea_d(in), true); break;
    case Op::stwx: use(rb); store("st32", ea_x(in), false); break;
    case Op::stwux: use(rb); store("st32", ea_x(in), true); break;
    case Op::sthbrx: use(rb); store("st16r", ea_x(in), false); break;
    case Op::stwbrx: use(rb); store("st32r", ea_x(in), false); break;
    case Op::lmw:
    case Op::stmw: {
        if (ra) use(ra);
        std::string code = std::format("{{ const uint32_t ea = {}; QILIN_PC({});", ea_d(in), hex(pc));
        for (unsigned r = rd; r < 32; ++r) {
            use(r);
            code += in.op == Op::lmw ? std::format(" r{} = rt::ld32(ea + {});", r, 4 * (r - rd))
                                     : std::format(" rt::st32(ea + {}, r{});", 4 * (r - rd), r);
        }
        line(code + " }");
        break;
    }
    case Op::lswi:
    case Op::stswi: {
        if (ra) use(ra);
        const unsigned n = rb ? rb : 32;
        std::string code = std::format("{{ const uint32_t ea = {}; QILIN_PC({});", RA0(ra), hex(pc));
        for (unsigned i = 0; i < n; ++i) {
            const unsigned r = (rd + i / 4) % 32, shift = 24 - 8 * (i % 4);
            use(r);
            if (in.op == Op::lswi)
                code += i % 4 == 0 ? std::format(" r{} = uint32_t(rt::ld8(ea + {})) << {};", r, i, shift)
                                   : std::format(" r{} |= uint32_t(rt::ld8(ea + {})) << {};", r, i, shift);
            else
                code += std::format(" rt::st8(ea + {}, r{} >> {});", i, r, shift);
        }
        line(code + " }");
        break;
    }
    case Op::lswx:
    case Op::stswx:
        // The byte count is in XER at run time: through the context.
        for (unsigned r = 0; r < 32; ++r) use(r);
        out_ = in_ = true;
        line(std::format("{{ const uint32_t ea = {}; QILIN_PC({}); out(); rt::{}(c, {}, ea); in(); }}", ea_x(in), hex(pc),
                         in.op == Op::lswx ? "lswx" : "stswx", rd));
        break;
    case Op::lwarx:
        use(rb), use(rd);
        if (ra) use(ra);
        line(std::format("{{ const uint32_t ea = {}; QILIN_PC({}); {} = rt::ld32(ea); c.reserve = 1; c.reserve_addr = ea; }}",
                         ea_x(in), hex(pc), D));
        break;
    case Op::stwcx_rc:
        use(rb), use(rd);
        if (ra) use(ra);
        line(std::format("{{ const uint32_t ea = {}; QILIN_PC({}); const bool ok = c.reserve && c.reserve_addr == ea; "
                         "if (ok) rt::st32(ea, {}); c.reserve = 0; cr = rt::set_crf(cr, 0, (ok ? rt::kEq : 0u) | rt::so_bit(xer)); }}",
                         ea_x(in), hex(pc), S));
        break;
    case Op::dcbz:
        use(rb);
        if (ra) use(ra);
        line(std::format("QILIN_PC({}); rt::dcbz({});", hex(pc), ea_x(in)));
        break;
    case Op::dcbf:
    case Op::dcbst:
    case Op::dcbt:
    case Op::dcbtst:
    case Op::icbi:
    case Op::sync:
    case Op::isync:
    case Op::eieio:
        line(std::format("// {}: no caches or reordering to manage", s.name));
        break;
    // ---- condition register ----
    case Op::crand: crop("a & b"); break;
    case Op::crandc: crop("a & ~b"); break;
    case Op::creqv: crop("~(a ^ b)"); break;
    case Op::crnand: crop("~(a & b)"); break;
    case Op::crnor: crop("~(a | b)"); break;
    case Op::cror: crop("a | b"); break;
    case Op::crorc: crop("a | ~b"); break;
    case Op::crxor: crop("a ^ b"); break;
    case Op::mcrf: line(std::format("cr = rt::set_crf(cr, {}, (cr >> {}) & 0xFu);", in.crfd(), 28 - 4 * in.crfs())); break;
    case Op::mcrxr: line(std::format("cr = rt::set_crf(cr, {}, xer >> 28); xer &= 0x0FFFFFFFu;", in.crfd())); break;
    case Op::mfcr: use(rd); line(std::format("{} = cr;", D)); break;
    case Op::mtcrf: {
        uint32_t m = 0;
        for (unsigned f = 0; f < 8; ++f)
            if (in.crm() & (0x80u >> f)) m |= 0xF0000000u >> (4 * f);
        use(rd);
        line(std::format("cr = (cr & {}) | ({} & {});", hex(~m), S, hex(m)));
        break;
    }
    // ---- special-purpose registers ----
    case Op::mfspr:
        use(rd);
        if (in.spr() == 1) line(std::format("{} = xer;", D));
        else if (in.spr() == 8) line(std::format("{} = lr;", D));
        else if (in.spr() == 9) line(std::format("{} = ctr;", D));
        else line(std::format("{} = rt::mfspr(c, {}, {});", D, in.spr(), hex(pc)));
        break;
    case Op::mtspr:
        use(rd);
        if (in.spr() == 1) line(std::format("xer = {} & 0xE000007Fu;", S));
        else if (in.spr() == 8) line(std::format("lr = {};", S));
        else if (in.spr() == 9) line(std::format("ctr = {};", S));
        else line(std::format("rt::mtspr(c, {}, {}, {});", in.spr(), S, hex(pc)));
        break;
    case Op::mftb: use(rd); line(std::format("{} = rt::mftb(c, {});", D, in.spr())); break;
    // ---- system ----
    case Op::sc:
        out_ = in_ = true;
        for (unsigned r = 0; r < 32; ++r) use(r);
        line(std::format("out(); rt::syscall(c, {}); in();", hex(pc)));
        break;
    case Op::tw:
    case Op::twi: {
        use(ra);
        const std::string a = A, b = in.op == Op::tw ? (use(rb), B) : imm;
        const unsigned to = rd;
        if (to == 31) {
            line(std::format("rt::trap({});", hex(pc)));
            break;
        }
        std::string cond;
        auto add = [&](const std::string& c) { cond += cond.empty() ? c : " || " + c; };
        if (to & 16) add(std::format("static_cast<int32_t>({}) < static_cast<int32_t>({})", a, b));
        if (to & 8) add(std::format("static_cast<int32_t>({}) > static_cast<int32_t>({})", a, b));
        if (to & 4) add(std::format("{} == {}", a, b));
        if (to & 2) add(std::format("{} < {}", a, b));
        if (to & 1) add(std::format("{} > {}", a, b));
        if (!cond.empty()) line(std::format("if ({}) rt::trap({});", cond, hex(pc)));
        break;
    }
    // ---- branches ----
    case Op::b:
    case Op::bc: {
        const uint32_t target = in.branch_target(pc);
        if (in.lk()) {
            if (target == pc + 4) {  // bl to the next instruction: the PC into LR
                line(std::format("lr = {};", hex(pc + 4)));
                break;
            }
            const std::string cond = in.op == Op::bc ? branch_cond(in, pc) : "";
            const std::string call = call_direct(pc, target);
            line(cond.empty() ? call : std::format("lr = {}; if ({}) {{ {} }}", hex(pc + 4), cond, call));
            break;
        }
        const std::string cond = in.op == Op::bc ? branch_cond(in, pc) : "";
        std::string go;
        if (f_.tails.contains(target)) {
            go = tail_direct(target);
        } else if (std::ranges::binary_search(f_.insns, target)) {
            go = std::format("goto {};", label(target));
            targets_.insert(target);
        } else {
            throw Unsupported{std::format("branch to {:08X}, outside the function and not a tail call", target)};
        }
        line(cond.empty() ? go : std::format("if ({}) {{ {} }}", cond, go));
        break;
    }
    case Op::bclr: {
        const std::string cond = branch_cond(in, pc);
        std::string go;
        if (in.lk()) {
            line(std::format("{{ const uint32_t t = lr; lr = {};", hex(pc + 4)));
            go = call_indirect(pc, "t");
            line(cond.empty() ? go + " }" : std::format("if ({}) {{ {} }} }}", cond, go));
            break;
        }
        go = ret();
        line(cond.empty() ? go : std::format("if ({}) {{ {} }}", cond, go));
        break;
    }
    case Op::bcctr: {
        const std::string cond = branch_cond(in, pc);
        if (in.lk()) {
            line(std::format("lr = {};", hex(pc + 4)));
            const std::string go = call_indirect(pc, "ctr");
            line(cond.empty() ? go : std::format("if ({}) {{ {} }}", cond, go));
            break;
        }
        const auto jt = std::ranges::find(f_.jtabs, pc, &JumpTable::at);
        if (jt != f_.jtabs.end() && cond.empty()) {
            std::string sw = "switch (ctr) {";
            for (uint32_t t : jt->targets) {
                sw += std::format(" case {}: goto {};", hex(t), label(t));
                targets_.insert(t);
            }
            line(sw + std::format(" default: rt::bad_jump({}, ctr); }}", hex(pc)));
            break;
        }
        out_ = true;
        const std::string go = std::format("out(); rt::call(c, ctr, {}); return;", hex(pc));
        line(cond.empty() ? go : std::format("if ({}) {{ {} }}", cond, go));
        break;
    }
    default:
        throw Unsupported{std::format("not in slice 1: {}", s.name)};
    }
}

LiftResult Emitter::run() {
    LiftResult r;
    uint32_t prev = 0;
    std::vector<std::pair<uint32_t, size_t>> label_spots;  // address, offset in body_
    for (uint32_t pc : f_.insns) {
        const Insn in = cfg_.insn(pc);
        label_spots.emplace_back(pc, body_.size());
        try {
            if (!in.valid()) throw Unsupported{"invalid instruction"};
            instruction(pc, in);
        } catch (const Unsupported& u) {
            r.fail_at = pc;
            r.why = u.why;
            return r;
        }
        prev = pc;
    }
    // Execution that runs on into the next function.
    if (!f_.insns.empty() && !Cfg::is_terminator(cfg_.insn(prev))) {
        const Insn last = cfg_.insn(prev);
        const bool always = (last.op == Op::bc || last.op == Op::bclr || last.op == Op::bcctr) && !last.lk() &&
                            (last.rd() & 0x14) == 0x14;
        if (!always) {
            if (!f_.falls_into) {
                r.fail_at = prev;
                r.why = "runs off the end of the function";
                return r;
            }
            line(tail_direct(f_.falls_into));
        }
    }

    // Labels where something jumps, inserted from the end so offsets stay valid.
    for (auto it = label_spots.rbegin(); it != label_spots.rend(); ++it)
        if (targets_.contains(it->first)) body_.insert(it->second, label(it->first) + ":;\n");

    std::string code = std::format("void {}(rt::Context& c) {{\n", fn_name(f_.start));
    // One declaration per local: MSVC applies [[maybe_unused]] only to the
    // first declarator of a declaration (C4189 on the rest).
    if (!gprs_.empty()) {
        code += "   ";
        for (unsigned g : gprs_) code += std::format(" [[maybe_unused]] uint32_t r{0} = c.r[{0}];", g);
        code += "\n";
    }
    code += "    [[maybe_unused]] uint32_t lr = c.lr; [[maybe_unused]] uint32_t ctr = c.ctr;"
            " [[maybe_unused]] uint32_t cr = c.cr; [[maybe_unused]] uint32_t xer = c.xer;\n";
    auto sync = [&](bool to_context) {
        std::string s;
        for (unsigned g : gprs_) s += to_context ? std::format(" c.r[{0}] = r{0};", g) : std::format(" r{0} = c.r[{0}];", g);
        s += to_context ? " c.lr = lr; c.ctr = ctr; c.cr = cr; c.xer = xer;" : " lr = c.lr; ctr = c.ctr; cr = c.cr; xer = c.xer;";
        return s;
    };
    if (out_) code += "    auto out = [&] {" + sync(true) + " };\n";
    if (in_) code += "    auto in = [&] {" + sync(false) + " };\n";
    code += body_ + "}\n";
    r.ok = true;
    r.code = std::move(code);
    return r;
}

}  // namespace

LiftResult Lifter::lift(const Function& f) const { return Emitter(cfg_, f).run(); }

}  // namespace qilin
