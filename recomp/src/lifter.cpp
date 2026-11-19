#include "lifter.hpp"

#include "hwaccess.hpp"

#include <algorithm>
#include <bitset>
#include <cctype>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

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
    void floating(uint32_t pc, const Insn& in);
    void paired(uint32_t pc, const Insn& in);
    void use(unsigned r) { gprs_.insert(r); }
    std::string F(unsigned n) {  // an FPR (its ps0)
        fprs_.insert(n);
        return std::format("f{}", n);
    }
    std::string P(unsigned n) {  // an FPR's ps1
        ps1_.insert(n);
        return std::format("p{}", n);
    }

    // Stores to the GX write-gather pipe: the runtime takes them
    // (rt::fifo_st8/16/32) instead of memory. Found by hwaccess's constant
    // propagation: the SDK's inline vertex functions write it at a constant
    // address from the caller.
    std::string store_fn(uint32_t pc, std::string_view fn) const {
        return fifo_.contains(pc) ? "fifo_" + std::string(fn) : std::string(fn);
    }

    const Cfg& cfg_;
    const Function& f_;
    std::set<uint32_t> fifo_;
    std::string body_;
    std::set<unsigned> gprs_, fprs_, ps1_;
    std::set<uint32_t> targets_;  // labels that something jumps to
    // The instruction being emitted: where it can jump, and whether it can't
    // go on to the next one. For the register dataflow in run().
    std::vector<uint32_t> jumps_;
    bool stops_ = false;
};

std::string Emitter::ea_d(const Insn& in) const {
    const uint32_t d = static_cast<uint32_t>(in.simm());
    return in.ra() ? std::format("r{} + {}", in.ra(), hex(d)) : hex(d);
}

std::string Emitter::ea_x(const Insn& in) const {
    return in.ra() ? std::format("r{} + r{}", in.ra(), in.rb()) : R(in.rb());
}

std::string Emitter::ret() {
    return "@OUT@ c.ret = lr; return;";
}

std::string Emitter::call_direct(uint32_t pc, uint32_t target) {
    return std::format("lr = {0}; @OUT@ {1}(c); @IN@ if (c.ret != {0}) rt::bad_return({2}, c.ret);", hex(pc + 4),
                       fn_name(target), hex(pc));
}

std::string Emitter::call_indirect(uint32_t pc, const std::string& target) {
    return std::format("@OUT@ rt::call(c, {1}, {2}); @IN@ if (c.ret != {0}) rt::bad_return({2}, c.ret);", hex(pc + 4),
                       target, hex(pc));
}

std::string Emitter::tail_direct(uint32_t target) {
    return std::format("@OUT@ {}(c); return;", fn_name(target));
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
    if (s.flags & ppc::kPaired) return paired(pc, in);
    if (s.flags & ppc::kFloat) return floating(pc, in);

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
        if (fifo_.contains(pc) && (fn.ends_with("r") || update))
            throw Unsupported{std::format("{} to the GX FIFO (only plain stores are taken over)", s.name)};
        line(std::format("{{ const uint32_t ea = {}; QILIN_PC({}); rt::{}(ea, {});{} }}", ea, hex(pc), store_fn(pc, fn), S,
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
        line(std::format("{{ const uint32_t ea = {}; QILIN_PC({}); @OUT@ rt::{}(c, {}, ea); @IN@ }}", ea_x(in), hex(pc),
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
        // A line written back: the GPU may read it now (rt::cache_store
        // tells the renderer's caches; the differential test ignores it).
        use(rb);
        if (ra) use(ra);
        line(std::format("rt::cache_store({});", ea_x(in)));
        break;
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
        for (unsigned r = 0; r < 32; ++r) use(r);
        line(std::format("@OUT@ rt::syscall(c, {}); @IN@", hex(pc)));
        break;
    case Op::tw:
    case Op::twi: {
        use(ra);
        const std::string a = A, b = in.op == Op::tw ? (use(rb), B) : imm;
        const unsigned to = rd;
        if (to == 31) {
            stops_ = true;
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
            jumps_.push_back(target);
            // A backward branch closes a loop: a safe point, with the loop's
            // stack pointer, so a loop waiting on an interrupt gets it.
            if (target <= pc) {
                use(1);
                line("if (rt::poll_due()) [[unlikely]] { c.r[1] = r1; rt::poll(c); }");
            }
        } else {
            throw Unsupported{std::format("branch to {:08X}, outside the function and not a tail call", target)};
        }
        line(cond.empty() ? go : std::format("if ({}) {{ {} }}", cond, go));
        stops_ = cond.empty();
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
        stops_ = cond.empty();
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
                jumps_.push_back(t);
            }
            stops_ = true;
            line(sw + std::format(" default: rt::bad_jump({}, ctr); }}", hex(pc)));
            break;
        }
        const std::string go = std::format("@OUT@ rt::call(c, ctr, {}); return;", hex(pc));
        line(cond.empty() ? go : std::format("if ({}) {{ {} }}", cond, go));
        stops_ = cond.empty();
        break;
    }
    default:
        throw Unsupported{std::format("not in slice 1: {}", s.name)};
    }
}

// Slice 3: floating point, through the helpers in guest.hpp (FPSCR, non-IEEE
// mode, NaNs and Broadway's results are described there). Only what the
// game's lifted code uses, and what is pinned down by a source, is lifted.
void Emitter::floating(uint32_t pc, const Insn& in) {
    const ppc::Spec& s = ppc::spec(in.op);
    if ((s.flags & ppc::kRc) && in.rc())
        throw Unsupported{std::format("floating-point record form (CR1 from FPSCR, not modelled): {}.", s.name)};
    const unsigned d = in.rd(), a = in.ra(), b = in.rb(), cc = in.rc_reg();

    auto load = [&](const std::string& ea, bool update, bool single) {
        if (a) use(a);
        // In paired-single mode lfs fills both halves; lfd writes ps0 only.
        line(std::format("{{ const uint32_t ea = {}; QILIN_PC({}); {} = {};{}{} }}", ea, hex(pc), F(d),
                         single ? "rt::lfs(rt::ld32(ea))" : "rt::dbl(rt::ld64(ea))",
                         single ? std::format(" {} = {};", P(d), F(d)) : "", update ? std::format(" {} = ea;", R(a)) : ""));
    };
    auto store = [&](const std::string& ea, bool update, const char* how) {
        if (a) use(a);
        const std::string v = F(d);
        if (fifo_.contains(pc) && (update || std::string(how) == "d"))
            throw Unsupported{std::format("{} to the GX FIFO (only stfs and stfiwx are taken over)", s.name)};
        const std::string st32 = store_fn(pc, "st32");
        const std::string st = std::string(how) == "s"   ? std::format("rt::{}(ea, rt::stfs({}))", st32, v)
                               : std::string(how) == "d" ? std::format("rt::st64(ea, rt::bits({}))", v)
                                                         : std::format("rt::{}(ea, static_cast<uint32_t>(rt::bits({})))", st32, v);
        line(std::format("{{ const uint32_t ea = {}; QILIN_PC({}); {};{} }}", ea, hex(pc), st,
                         update ? std::format(" {} = ea;", R(a)) : ""));
    };
    auto set = [&](const std::string& expr) { line(std::format("{} = {};", F(d), expr)); };
    // Single-precision results go to both halves in paired-single mode.
    auto set_s = [&](const std::string& expr) { line(std::format("{0} = {1}; {2} = {0};", F(d), expr, P(d))); };
    // A single-precision multiply: Broadway rounds frC first, by a rule no
    // source here pins down, so lifted code only goes on when that's a no-op.
    auto single_frc = [&] { line(std::format("if (!rt::single_significand({})) rt::fp_unknown({});", F(cc), hex(pc))); };
    auto x = [&](unsigned n) { return F(n); };

    switch (in.op) {
    case Op::lfs: load(ea_d(in), false, true); break;
    case Op::lfsu: load(ea_d(in), true, true); break;
    case Op::lfsx: use(b); load(ea_x(in), false, true); break;
    case Op::lfsux: use(b); load(ea_x(in), true, true); break;
    case Op::lfd: load(ea_d(in), false, false); break;
    case Op::lfdu: load(ea_d(in), true, false); break;
    case Op::lfdx: use(b); load(ea_x(in), false, false); break;
    case Op::lfdux: use(b); load(ea_x(in), true, false); break;
    case Op::stfs: store(ea_d(in), false, "s"); break;
    case Op::stfsu: store(ea_d(in), true, "s"); break;
    case Op::stfsx: use(b); store(ea_x(in), false, "s"); break;
    case Op::stfsux: use(b); store(ea_x(in), true, "s"); break;
    case Op::stfd: store(ea_d(in), false, "d"); break;
    case Op::stfdu: store(ea_d(in), true, "d"); break;
    case Op::stfdx: use(b); store(ea_x(in), false, "d"); break;
    case Op::stfdux: use(b); store(ea_x(in), true, "d"); break;
    case Op::stfiwx: use(b); store(ea_x(in), false, "w"); break;

    case Op::fadd: set(std::format("rt::fadd({}, {})", x(a), x(b))); break;
    case Op::fsub: set(std::format("rt::fsub({}, {})", x(a), x(b))); break;
    case Op::fmul: set(std::format("rt::fmul({}, {})", x(a), x(cc))); break;
    case Op::fdiv: set(std::format("rt::fdiv({}, {})", x(a), x(b))); break;
    case Op::fadds: set_s(std::format("rt::fadds({}, {})", x(a), x(b))); break;
    case Op::fsubs: set_s(std::format("rt::fsubs({}, {})", x(a), x(b))); break;
    case Op::fmuls: single_frc(); set_s(std::format("rt::fmuls({}, {})", x(a), x(cc))); break;
    case Op::fdivs: set_s(std::format("rt::fdivs({}, {})", x(a), x(b))); break;
    case Op::fmadd: set(std::format("rt::fma_({}, {}, {}, false, false)", x(a), x(cc), x(b))); break;
    case Op::fmsub: set(std::format("rt::fma_({}, {}, {}, true, false)", x(a), x(cc), x(b))); break;
    case Op::fnmadd: set(std::format("rt::fma_({}, {}, {}, false, true)", x(a), x(cc), x(b))); break;
    case Op::fnmsub: set(std::format("rt::fma_({}, {}, {}, true, true)", x(a), x(cc), x(b))); break;
    case Op::fmadds: single_frc(); set_s(std::format("rt::fmas({}, {}, {}, false, false)", x(a), x(cc), x(b))); break;
    case Op::fmsubs: single_frc(); set_s(std::format("rt::fmas({}, {}, {}, true, false)", x(a), x(cc), x(b))); break;
    case Op::fnmadds: single_frc(); set_s(std::format("rt::fmas({}, {}, {}, false, true)", x(a), x(cc), x(b))); break;
    case Op::fnmsubs: single_frc(); set_s(std::format("rt::fmas({}, {}, {}, true, true)", x(a), x(cc), x(b))); break;
    case Op::frsp: set_s(std::format("rt::frsp({})", x(b))); break;
    case Op::fctiwz: set(std::format("rt::fctiwz({})", x(b))); break;
    case Op::fmr: set(x(b)); break;
    case Op::fneg: set(std::format("rt::fneg({})", x(b))); break;
    case Op::fabs: set(std::format("rt::fabs_({})", x(b))); break;
    case Op::fnabs: set(std::format("rt::fneg(rt::fabs_({}))", x(b))); break;
    case Op::fcmpu:
    case Op::fcmpo:
        // Non-IEEE mode zeroes denormal operands of arithmetic; for compares
        // no source says, so a denormal operand stops.
        line(std::format("if (rt::denormal_d({0}) || rt::denormal_d({1})) rt::fp_unknown({2}); "
                         "cr = rt::set_crf(cr, {3}, rt::fcmp({0}, {1}));",
                         x(a), x(b), hex(pc), in.crfd()));
        break;
    case Op::fsel:
        // frA >= 0 (either zero included) selects frC, anything else (NaN
        // too) frB. A denormal frA under non-IEEE mode could be read either
        // way, so it stops. ps1 is undefined after fsel in the manual; as a
        // double-precision instruction, it leaves it alone.
        line(std::format("if (rt::denormal_d({0})) rt::fp_unknown({1}); {2} = {0} >= 0.0 ? {3} : {4};", x(a), hex(pc), F(d),
                         x(cc), x(b)));
        break;
    // The estimates: exact where the architecture defines them (zeros,
    // infinities, NaNs, negative square roots); anything that needs
    // Broadway's table stops (rt::frsqrte, rt::fres).
    case Op::frsqrte: set(std::format("rt::frsqrte({}, {})", x(b), hex(pc))); break;
    case Op::fres: set_s(std::format("rt::fres({}, {})", x(b), hex(pc))); break;
    case Op::mffs:
    case Op::mtfsf:
    case Op::mtfsfi:
    case Op::mtfsb0:
    case Op::mtfsb1:
    case Op::mcrfs:
        throw Unsupported{std::format("FPSCR access (only in replaced code; FPSCR not modelled): {}", s.name)};
    default:
        throw Unsupported{std::format("floating point not used by the game's lifted code: {}", s.name)};
    }
}

// Slice 4: paired singles. Each lane works as the single-precision
// instruction of the same name does (rt::fadds and friends: non-IEEE mode,
// NaNs, rounding to single); a pair is computed into temporaries before
// either half is written, since frD may also be a source.
void Emitter::paired(uint32_t pc, const Insn& in) {
    const ppc::Spec& s = ppc::spec(in.op);
    if ((s.flags & ppc::kRc) && in.rc())
        throw Unsupported{std::format("paired-single record form (CR1 from FPSCR, not modelled): {}.", s.name)};
    const unsigned d = in.rd(), a = in.ra(), b = in.rb(), cc = in.rc_reg();
    auto pair = [&](const std::string& x0, const std::string& x1) {
        line(std::format("{{ const double x0 = {}, x1 = {}; {} = x0; {} = x1; }}", x0, x1, F(d), P(d)));
    };
    // The multiplier lanes a multiply uses must be singles (see single_frc in floating()).
    auto frc = [&](const std::vector<std::string>& lanes) {
        std::string cond;
        for (const std::string& l : lanes) cond += (cond.empty() ? "" : " || ") + std::format("!rt::single_significand({})", l);
        line(std::format("if ({}) rt::fp_unknown({});", cond, hex(pc)));
    };
    auto quant = [&](bool load, const std::string& ea, unsigned w, unsigned i, bool update) {
        if (a) use(a);
        const std::string gqr = std::format("c.spr[{}]", 912 + i);
        const std::string op = load ? std::format("rt::psq_l({}, {}, ea, {}, {}, {});", F(d), P(d), w ? "true" : "false", gqr, hex(pc))
                                    : std::format("rt::{}({}, {}, ea, {}, {}, {});", store_fn(pc, "psq_st"), F(d), P(d),
                                                  w ? "true" : "false", gqr, hex(pc));
        line(std::format("{{ const uint32_t ea = {}; QILIN_PC({}); {}{} }}", ea, hex(pc), op,
                         update ? std::format(" {} = ea;", R(a)) : ""));
    };
    const std::string ea12 = a ? std::format("r{} + {}", a, hex(static_cast<uint32_t>(in.psq_d()))) : hex(static_cast<uint32_t>(in.psq_d()));
    auto cmp = [&](const std::string& x, const std::string& y) {
        line(std::format("if (rt::denormal_d({0}) || rt::denormal_d({1})) rt::fp_unknown({2}); "
                         "cr = rt::set_crf(cr, {3}, rt::fcmp({0}, {1}));",
                         x, y, hex(pc), in.crfd()));
    };
    // Registers are named (and so declared) only where an instruction uses them.
    auto fa = [&] { return F(a); };
    auto fb = [&] { return F(b); };
    auto fc = [&] { return F(cc); };
    auto pa = [&] { return P(a); };
    auto pb = [&] { return P(b); };
    auto pc_ = [&] { return P(cc); };
    switch (in.op) {
    case Op::psq_l: quant(true, ea12, in.psq_w(), in.psq_i(), false); break;
    case Op::psq_lu: quant(true, ea12, in.psq_w(), in.psq_i(), true); break;
    case Op::psq_st: quant(false, ea12, in.psq_w(), in.psq_i(), false); break;
    case Op::psq_stu: quant(false, ea12, in.psq_w(), in.psq_i(), true); break;
    case Op::psq_lx: use(b); quant(true, ea_x(in), in.psqx_w(), in.psqx_i(), false); break;
    case Op::psq_lux: use(b); quant(true, ea_x(in), in.psqx_w(), in.psqx_i(), true); break;
    case Op::psq_stx: use(b); quant(false, ea_x(in), in.psqx_w(), in.psqx_i(), false); break;
    case Op::psq_stux: use(b); quant(false, ea_x(in), in.psqx_w(), in.psqx_i(), true); break;

    case Op::ps_add: pair(std::format("rt::fadds({}, {})", fa(), fb()), std::format("rt::fadds({}, {})", pa(), pb())); break;
    case Op::ps_sub: pair(std::format("rt::fsubs({}, {})", fa(), fb()), std::format("rt::fsubs({}, {})", pa(), pb())); break;
    case Op::ps_div: pair(std::format("rt::fdivs({}, {})", fa(), fb()), std::format("rt::fdivs({}, {})", pa(), pb())); break;
    case Op::ps_mul:
        frc({fc(), pc_()});
        pair(std::format("rt::fmuls({}, {})", fa(), fc()), std::format("rt::fmuls({}, {})", pa(), pc_()));
        break;
    case Op::ps_muls0:
        frc({fc()});
        pair(std::format("rt::fmuls({}, {})", fa(), fc()), std::format("rt::fmuls({}, {})", pa(), fc()));
        break;
    case Op::ps_muls1:
        frc({pc_()});
        pair(std::format("rt::fmuls({}, {})", fa(), pc_()), std::format("rt::fmuls({}, {})", pa(), pc_()));
        break;
    case Op::ps_madd:
    case Op::ps_msub:
    case Op::ps_nmadd:
    case Op::ps_nmsub: {
        const bool sub = in.op == Op::ps_msub || in.op == Op::ps_nmsub, neg = in.op == Op::ps_nmadd || in.op == Op::ps_nmsub;
        frc({fc(), pc_()});
        pair(std::format("rt::fmas({}, {}, {}, {}, {})", fa(), fc(), fb(), sub, neg),
             std::format("rt::fmas({}, {}, {}, {}, {})", pa(), pc_(), pb(), sub, neg));
        break;
    }
    case Op::ps_madds0:
        frc({fc()});
        pair(std::format("rt::fmas({}, {}, {}, false, false)", fa(), fc(), fb()), std::format("rt::fmas({}, {}, {}, false, false)", pa(), fc(), pb()));
        break;
    case Op::ps_madds1:
        frc({pc_()});
        pair(std::format("rt::fmas({}, {}, {}, false, false)", fa(), pc_(), fb()), std::format("rt::fmas({}, {}, {}, false, false)", pa(), pc_(), pb()));
        break;
    case Op::ps_sum0: pair(std::format("rt::fadds({}, {})", fa(), pb()), pc_()); break;
    case Op::ps_sum1: pair(fc(), std::format("rt::fadds({}, {})", fa(), pb())); break;
    case Op::ps_merge00: pair(fa(), fb()); break;
    case Op::ps_merge01: pair(fa(), pb()); break;
    case Op::ps_merge10: pair(pa(), fb()); break;
    case Op::ps_merge11: pair(pa(), pb()); break;
    case Op::ps_mr: pair(fb(), pb()); break;
    case Op::ps_neg: pair(std::format("rt::fneg({})", fb()), std::format("rt::fneg({})", pb())); break;
    case Op::ps_abs: pair(std::format("rt::fabs_({})", fb()), std::format("rt::fabs_({})", pb())); break;
    case Op::ps_nabs: pair(std::format("rt::fneg(rt::fabs_({}))", fb()), std::format("rt::fneg(rt::fabs_({}))", pb())); break;
    case Op::ps_cmpu0:
    case Op::ps_cmpo0: cmp(fa(), fb()); break;
    case Op::ps_cmpu1:
    case Op::ps_cmpo1: cmp(pa(), pb()); break;
    case Op::ps_res:
    case Op::ps_rsqrte:
        throw Unsupported{std::format("floating-point estimate (Broadway's table not known): {}", s.name)};
    default:
        throw Unsupported{std::format("paired single not used by the game's lifted code: {}", s.name)};
    }
}

// ---- Register dataflow around sync points ----
// Locals cache the guest registers; the context holds them where something
// outside the function looks: before a call, a return, a tail call (the
// @OUT@ markers in the emitted text) and after a call (@IN@). Two passes
// decide what each sync point moves, so most calls move a few registers
// instead of every one the function uses:
//  - dirty (forward, may): written since the last sync on some path. Only
//    these are stored at @OUT@; the context already holds the rest.
//  - live (backward): read before being overwritten, where a store at a
//    later @OUT@ counts as a read. Only these are reloaded at @IN@; any other
//    local is stale after the call but is overwritten before anything reads
//    or stores it.
// The emitted text is scanned for the registers it mentions: any mention
// dirties a register and counts as a read, and only a plain assignment that
// isn't inside a condition kills. Both approximations err on the safe side,
// a store or a reload too many.
constexpr unsigned kRegCount = 100;  // r0-31, f0-31 (ps0), p0-31 (ps1), lr, ctr, cr, xer
using Regs = std::bitset<kRegCount>;
constexpr unsigned kLr = 96, kCtr = 97, kCr = 98, kXer = 99;

int reg_index(std::string_view id) {
    if (id == "lr") return kLr;
    if (id == "ctr") return kCtr;
    if (id == "cr") return kCr;
    if (id == "xer") return kXer;
    if (id.size() < 2 || id.size() > 3 || (id[0] != 'r' && id[0] != 'f' && id[0] != 'p')) return -1;
    unsigned n = 0;
    for (char ch : id.substr(1)) {
        if (ch < '0' || ch > '9') return -1;
        n = n * 10 + unsigned(ch - '0');
    }
    if (n > 31 || (id.size() == 3 && id[1] == '0')) return -1;
    return static_cast<int>((id[0] == 'r' ? 0 : id[0] == 'f' ? 32 : 64) + n);
}

std::string local_name(unsigned i) {
    if (i >= 96) return i == kLr ? "lr" : i == kCtr ? "ctr" : i == kCr ? "cr" : "xer";
    return std::format("{}{}", i < 32 ? 'r' : i < 64 ? 'f' : 'p', i % 32);
}

std::string context_name(unsigned i) {
    if (i >= 96) return "c." + local_name(i);
    return std::format("c.{}[{}]", i < 32 ? "r" : i < 64 ? "f" : "ps1", i % 32);
}

bool word_char(char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'; }

// The registers a piece of generated code mentions (members like c.lr and
// names like rt::crb aren't registers).
Regs mentions(std::string_view t) {
    Regs m;
    for (size_t i = 0; i < t.size();) {
        if (!word_char(t[i])) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < t.size() && word_char(t[j])) ++j;
        const char before = i ? t[i - 1] : ' ';
        if (before != '.' && before != ':' && !std::isdigit(static_cast<unsigned char>(t[i])))
            if (const int r = reg_index(t.substr(i, j - i)); r >= 0) m.set(static_cast<size_t>(r));
        i = j;
    }
    return m;
}

// The registers a piece of generated code may write: assignment targets
// (=, compound assignments, ++ and --) and the arguments of the runtime
// helpers that take locals by reference (only rt::psq_l; a new one must be
// added here, or the dataflow would skip storing what it writes).
Regs writes(std::string_view t) {
    Regs w;
    if (t.find("rt::psq_l(") != std::string_view::npos) return mentions(t);
    for (size_t i = 0; i < t.size();) {
        if (!word_char(t[i])) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < t.size() && word_char(t[j])) ++j;
        const char before = i ? t[i - 1] : ' ';
        const int r = before != '.' && before != ':' && !std::isdigit(static_cast<unsigned char>(t[i]))
                          ? reg_index(t.substr(i, j - i))
                          : -1;
        if (r >= 0) {
            size_t k = j;
            while (k < t.size() && t[k] == ' ') ++k;
            const std::string_view after = t.substr(k);
            const bool assigned = (after.starts_with("=") && !after.starts_with("==")) || after.starts_with("|=") ||
                                  after.starts_with("&=") || after.starts_with("^=") || after.starts_with("+=") ||
                                  after.starts_with("-=") || after.starts_with("<<=") || after.starts_with(">>=") ||
                                  after.starts_with("++") || after.starts_with("--");
            const bool stepped = i >= 2 && (t.substr(i - 2, 2) == "--" || t.substr(i - 2, 2) == "++");
            if (assigned || stepped) w.set(static_cast<size_t>(r));
        }
        i = j;
    }
    return w;
}

// What a piece of code reads before writing (gen) and certainly writes
// (kill), statement by statement in order. A statement "x = expr" reads expr
// and writes x; any other statement reads everything it mentions. From the
// first condition on, everything mentioned is read and nothing counts as
// written.
struct UseDef {
    Regs gen, kill;
};
UseDef use_def(std::string_view t) {
    UseDef u;
    std::string_view rest;
    for (const std::string_view stop : {"if (", "switch ("})
        if (const size_t at = t.find(stop); at != std::string_view::npos) {
            rest = t.substr(at);
            t = t.substr(0, at);
        }
    size_t start = 0;
    for (size_t i = 0; i <= t.size(); ++i) {
        if (i < t.size() && t[i] != ';' && t[i] != '{' && t[i] != '}') continue;
        std::string_view st = t.substr(start, i - start);
        start = i + 1;
        while (!st.empty() && (st.front() == ' ' || st.front() == '\n')) st.remove_prefix(1);
        size_t j = 0;
        while (j < st.size() && word_char(st[j])) ++j;
        const int r = reg_index(st.substr(0, j));
        if (r >= 0 && st.substr(j).starts_with(" = ")) {
            u.gen |= mentions(st.substr(j + 3)) & ~u.kill;
            u.kill.set(static_cast<size_t>(r));
        } else {
            u.gen |= mentions(st) & ~u.kill;
        }
    }
    u.gen |= mentions(rest) & ~u.kill;
    return u;
}

LiftResult Emitter::run() {
    LiftResult r;
    struct Node {
        uint32_t pc = 0;
        std::string text;
        std::vector<uint32_t> jumps;
        bool stops = false;
    };
    std::vector<Node> nodes;
    for (const HwAccess& h : hardware_accesses(cfg_, f_))
        if (h.store && hw_region(h.ea) == "GX FIFO (write-gather pipe)") fifo_.insert(h.pc);
    for (uint32_t pc : f_.insns) {
        const Insn in = cfg_.insn(pc);
        body_.clear();
        jumps_.clear();
        stops_ = false;
        try {
            if (!in.valid()) throw Unsupported{"invalid instruction"};
            instruction(pc, in);
        } catch (const Unsupported& u) {
            r.fail_at = pc;
            r.why = u.why;
            return r;
        }
        nodes.push_back({pc, std::move(body_), jumps_, stops_});
    }
    // Execution that runs on into the next function.
    if (!nodes.empty() && !nodes.back().stops) {
        const uint32_t prev = nodes.back().pc;
        const Insn last = cfg_.insn(prev);
        const bool always = (last.op == Op::bc || last.op == Op::bclr || last.op == Op::bcctr) && !last.lk() &&
                            (last.rd() & 0x14) == 0x14;
        if (!Cfg::is_terminator(last) && !always) {
            if (!f_.falls_into) {
                r.fail_at = prev;
                r.why = "runs off the end of the function";
                return r;
            }
            body_.clear();
            line(tail_direct(f_.falls_into));
            nodes.push_back({0, std::move(body_), {}, true});
        }
    }

    // Each node: the text before @OUT@, between @OUT@ and @IN@ (or the end),
    // and after @IN@; whether its sync is conditional; its successors.
    const size_t n = nodes.size();
    std::map<uint32_t, size_t> at;
    for (size_t i = 0; i < n; ++i)
        if (nodes[i].pc) at[nodes[i].pc] = i;
    struct Part {
        bool out = false, in = false, conditional = false;
        Regs pre_w, mid_m, post_w;  // what may be written (for dirty); what the call reads (mid)
        UseDef pre, post;           // reads and certain writes (for live)
        std::vector<size_t> succ;
    };
    std::vector<Part> part(n);
    for (size_t i = 0; i < n; ++i) {
        const std::string_view t = nodes[i].text;
        Part& p = part[i];
        const size_t o = t.find("@OUT@"), g = t.find("@IN@");
        p.out = o != std::string_view::npos;
        p.in = g != std::string_view::npos;
        const std::string_view pre = t.substr(0, p.out ? o : t.size());
        p.conditional = p.out && pre.find("if (") != std::string_view::npos;
        p.pre_w = writes(pre), p.pre = use_def(pre);
        if (p.out) p.mid_m = mentions(t.substr(o + 5, p.in ? g - o - 5 : std::string_view::npos));
        if (p.in) p.post_w = writes(t.substr(g + 4)), p.post = use_def(t.substr(g + 4));
        if (!nodes[i].stops && i + 1 < n) p.succ.push_back(i + 1);
        for (uint32_t target : nodes[i].jumps) p.succ.push_back(at.at(target));
    }

    // Dirty, forward to a fixpoint.
    std::vector<Regs> dirty_in(n), dirty_out(n), dirty_at_out(n);
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < n; ++i) {
            const Part& p = part[i];
            Regs d = dirty_in[i] | p.pre_w;
            if (p.out) {
                dirty_at_out[i] = d;
                d = p.conditional ? d | p.post_w : p.post_w;
            }
            if (d != dirty_out[i]) dirty_out[i] = d, changed = true;
            for (size_t s : p.succ)
                if ((dirty_in[s] | d) != dirty_in[s]) dirty_in[s] |= d, changed = true;
        }
    }
    // Live, backward to a fixpoint.
    std::vector<Regs> live_in(n), reload(n);
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t k = n; k-- > 0;) {
            const Part& p = part[k];
            Regs l;
            for (size_t s : p.succ) l |= live_in[s];
            if (p.out) {
                Regs after = l;  // what's live once the call returns
                if (p.in) after = p.conditional ? after | p.post.gen : (after & ~p.post.kill) | p.post.gen;
                reload[k] = p.in ? after : Regs{};
                const Regs before_call = p.mid_m | dirty_at_out[k];
                l = p.conditional ? (l | after | before_call) : before_call;
            }
            l = (l & ~p.pre.kill) | p.pre.gen;
            if (l != live_in[k]) live_in[k] = l, changed = true;
        }
    }

    // The text, with each sync point's stores and reloads, and the labels.
    std::string body;
    for (size_t i = 0; i < n; ++i) {
        std::string t = std::move(nodes[i].text);
        if (nodes[i].pc && targets_.contains(nodes[i].pc)) body += label(nodes[i].pc) + ":;\n";
        auto sync = [](const Regs& set, bool store) {
            std::string s;
            for (unsigned g = 0; g < kRegCount; ++g)
                if (set[g])
                    s += store ? std::format("{} = {}; ", context_name(g), local_name(g))
                               : std::format("{} = {}; ", local_name(g), context_name(g));
            return s;
        };
        if (const size_t o = t.find("@OUT@ "); o != std::string::npos) t.replace(o, 6, sync(dirty_at_out[i], true));
        if (const size_t g = t.find("@IN@"); g != std::string::npos) {
            std::string s = sync(reload[i], false);
            if (g + 4 < t.size() && t[g + 4] == ' ') t.erase(g + 4, 1);
            t.replace(g, 4, s);
        }
        body += t;
    }

    std::string code = std::format("void {}(rt::Context& c) {{\n", fn_name(f_.start));
    // One declaration per local: MSVC applies [[maybe_unused]] only to the
    // first declarator of a declaration (C4189 on the rest).
    if (!gprs_.empty()) {
        code += "   ";
        for (unsigned g : gprs_) code += std::format(" [[maybe_unused]] uint32_t r{0} = c.r[{0}];", g);
        code += "\n";
    }
    if (!fprs_.empty()) {
        code += "   ";
        for (unsigned g : fprs_) code += std::format(" [[maybe_unused]] double f{0} = c.f[{0}];", g);
        code += "\n";
    }
    if (!ps1_.empty()) {
        code += "   ";
        for (unsigned g : ps1_) code += std::format(" [[maybe_unused]] double p{0} = c.ps1[{0}];", g);
        code += "\n";
    }
    code += "    [[maybe_unused]] uint32_t lr = c.lr; [[maybe_unused]] uint32_t ctr = c.ctr;"
            " [[maybe_unused]] uint32_t cr = c.cr; [[maybe_unused]] uint32_t xer = c.xer;\n";
    code += body + "}\n";
    r.ok = true;
    r.code = std::move(code);
    return r;
}

}  // namespace

LiftResult Lifter::lift(const Function& f) const { return Emitter(cfg_, f).run(); }

}  // namespace qilin
