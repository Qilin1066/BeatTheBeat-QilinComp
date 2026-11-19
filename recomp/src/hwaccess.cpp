#include "hwaccess.hpp"

#include <array>
#include <map>
#include <optional>

namespace qilin {

using ppc::Insn;
using ppc::Op;

// The Wii's address map as the PowerPC side sees it (the common, public
// layout): MEM1 and MEM2 cached and uncached, the embedded framebuffer, the
// locked cache, and the hardware registers at 0xCC000000 (GameCube-era
// blocks), 0xCD000000 (Hollywood, with the same blocks again from 0x6000)
// and the 0xCD800000 mirror.
std::string_view hw_region(uint32_t ea) {
    if (ea >= 0x80000000u && ea < 0x81800000u) return "";
    if (ea >= 0xC0000000u && ea < 0xC1800000u) return "MEM1 uncached";
    if (ea >= 0x90000000u && ea < 0x94000000u) return "MEM2";
    if (ea >= 0xD0000000u && ea < 0xD4000000u) return "MEM2 uncached";
    if (ea >= 0xC8000000u && ea < 0xC8400000u) return "EFB (embedded framebuffer)";
    if (ea >= 0xE0000000u && ea < 0xE0004000u) return "locked cache";
    if (ea >= 0xCD800000u && ea < 0xCD810000u) return "Hollywood registers (mirror)";
    const bool flipper = ea >= 0xCC000000u && ea < 0xCC010000u;
    const bool hollywood = ea >= 0xCD000000u && ea < 0xCD010000u;
    if (!flipper && !hollywood) return "unmapped";
    const uint32_t off = ea & 0xFFFF;
    if (hollywood && off < 0x6000) return "Hollywood registers (IPC and others)";
    if (off < 0x1000) return "CP (command processor)";
    if (off < 0x2000) return "PE (pixel engine)";
    if (off < 0x3000) return "VI (video interface)";
    if (off < 0x4000) return "PI (processor interface)";
    if (off < 0x5000) return "MI (memory interface)";
    if (off < 0x6000) return "DSP interface";
    if (off < 0x6400) return "DI (disc interface)";
    if (off < 0x6800) return "SI (serial interface)";
    if (off < 0x6C00) return "EXI (external interface)";
    if (off < 0x7000) return "AI (audio interface)";
    if (off >= 0x8000 && off < 0x9000 && flipper) return "GX FIFO (write-gather pipe)";
    return "unmapped";
}

namespace {

// Known constant GPRs at a point.
struct Regs {
    std::array<uint32_t, 32> v{};
    uint32_t known = 0;
    bool has(unsigned r) const { return (known >> r) & 1; }
    void set(unsigned r, uint32_t x) {
        v[r] = x;
        known |= 1u << r;
    }
    void clear(unsigned r) { known &= ~(1u << r); }
    bool operator==(const Regs& o) const {
        if (known != o.known) return false;
        for (unsigned r = 0; r < 32; ++r)
            if (has(r) && v[r] != o.v[r]) return false;
        return true;
    }
};

Regs meet(const Regs& a, const Regs& b) {
    Regs m;
    for (unsigned r = 0; r < 32; ++r)
        if (a.has(r) && b.has(r) && a.v[r] == b.v[r]) m.set(r, a.v[r]);
    return m;
}

bool update_form(Op op) {
    switch (op) {
    case Op::lbzu: case Op::lbzux: case Op::lhau: case Op::lhaux: case Op::lhzu: case Op::lhzux:
    case Op::lwzu: case Op::lwzux: case Op::stbu: case Op::stbux: case Op::sthu: case Op::sthux:
    case Op::stwu: case Op::stwux: case Op::lfsu: case Op::lfsux: case Op::lfdu: case Op::lfdux:
    case Op::stfsu: case Op::stfsux: case Op::stfdu: case Op::stfdux:
    case Op::psq_lu: case Op::psq_lux: case Op::psq_stu: case Op::psq_stux:
        return true;
    default:
        return false;
    }
}

bool accesses_memory(const Insn& in) {
    const uint32_t f = ppc::spec(in.op).flags;
    return (f & (ppc::kLoad | ppc::kStore)) || in.op == Op::dcbz || in.op == Op::dcbz_l;
}

bool is_store(const Insn& in) {
    return (ppc::spec(in.op).flags & ppc::kStore) || in.op == Op::dcbz || in.op == Op::dcbz_l;
}

uint32_t access_size(const Insn& in) {
    switch (in.op) {
    case Op::lbz: case Op::lbzu: case Op::lbzux: case Op::lbzx:
    case Op::stb: case Op::stbu: case Op::stbux: case Op::stbx:
        return 1;
    case Op::lha: case Op::lhau: case Op::lhaux: case Op::lhax: case Op::lhz: case Op::lhzu: case Op::lhzux:
    case Op::lhzx: case Op::lhbrx: case Op::sth: case Op::sthu: case Op::sthux: case Op::sthx: case Op::sthbrx:
        return 2;
    case Op::lfd: case Op::lfdu: case Op::lfdux: case Op::lfdx:
    case Op::stfd: case Op::stfdu: case Op::stfdux: case Op::stfdx:
    case Op::psq_l: case Op::psq_lu: case Op::psq_lux: case Op::psq_lx:
    case Op::psq_st: case Op::psq_stu: case Op::psq_stux: case Op::psq_stx:
        return 8;
    case Op::dcbz: case Op::dcbz_l:
        return 32;
    case Op::lmw: case Op::stmw:
        return 4 * (32 - in.rd());
    case Op::lswi: case Op::stswi:
        return in.rb() ? in.rb() : 32;
    case Op::lswx: case Op::stswx:
        return 0;  // the count is in XER at run time
    default:
        return 4;
    }
}

// The effective address, if the registers it uses are known.
std::optional<uint32_t> effective_address(const Insn& in, const Regs& r) {
    const unsigned ra = in.ra(), rb = in.rb();
    if (ra && !r.has(ra)) return std::nullopt;
    const uint32_t base = ra ? r.v[ra] : 0;
    if (in.op == Op::lswi || in.op == Op::stswi) return base;
    const unsigned p = in.primary();
    if (p == 31 || p == 4) {  // indexed: (rA|0) + rB
        if (!r.has(rb)) return std::nullopt;
        return base + r.v[rb];
    }
    if (p >= 56 && p <= 61) return base + static_cast<uint32_t>(in.psq_d());  // psq_l/psq_st
    return base + static_cast<uint32_t>(in.simm());
}

// What an instruction does to the known constants. Anything that may write
// a GPR, other than the forms followed here, makes it unknown.
void transfer(const Insn& in, Regs& r) {
    const unsigned rd = in.rd(), ra = in.ra(), rb = in.rb();
    if (update_form(in.op)) r.clear(ra);
    switch (in.op) {
    case Op::addi:
        if (!ra) r.set(rd, static_cast<uint32_t>(in.simm()));
        else if (r.has(ra)) r.set(rd, r.v[ra] + static_cast<uint32_t>(in.simm()));
        else r.clear(rd);
        return;
    case Op::addis:
        if (!ra) r.set(rd, in.uimm() << 16);
        else if (r.has(ra)) r.set(rd, r.v[ra] + (in.uimm() << 16));
        else r.clear(rd);
        return;
    case Op::ori:
    case Op::oris:
        if (r.has(rd)) r.set(ra, r.v[rd] | (in.op == Op::ori ? in.uimm() : in.uimm() << 16));
        else r.clear(ra);
        return;
    case Op::or_:  // mr rA, rS when rS == rB
        if (rd == rb && r.has(rd)) r.set(ra, r.v[rd]);
        else r.clear(ra);
        return;
    case Op::lmw:
        for (unsigned x = rd; x < 32; ++x) r.clear(x);
        return;
    case Op::lswi:
    case Op::lswx:
    case Op::sc:
        r.known = 0;
        return;
    case Op::b:
    case Op::bc:
    case Op::bclr:
    case Op::bcctr:
        // A call may change the volatile registers (r0, r3-r12). The EABI
        // that CodeWarrior follows keeps r1, r2, r13 and r14-r31: lifted
        // code relies on that (a FIFO address set up in r24, then GXBegin,
        // then the vertex stores through r24).
        if (in.lk()) {
            r.clear(0);
            for (unsigned x = 3; x <= 12; ++x) r.clear(x);
        }
        return;
    case Op::cmp: case Op::cmpi: case Op::cmpl: case Op::cmpli: case Op::mtcrf: case Op::mcrf: case Op::mcrxr:
    case Op::mtspr: case Op::mtmsr: case Op::mtsr: case Op::mtsrin: case Op::tw: case Op::twi: case Op::tlbie:
    case Op::tlbsync: case Op::rfi: case Op::stwcx_rc: case Op::ecowx:
        return;
    default:
        break;
    }
    const uint32_t f = ppc::spec(in.op).flags;
    if (f & (ppc::kCache | ppc::kSync | ppc::kStore)) return;
    if (f & ppc::kLoad) {
        if (!(f & (ppc::kFloat | ppc::kPaired))) r.clear(rd);
        return;
    }
    if (f & (ppc::kFloat | ppc::kPaired)) return;  // FPRs and FPSCR only
    r.clear(rd);  // everything else: assume both GPR fields may be written
    r.clear(ra);
}

}  // namespace

std::vector<HwAccess> hardware_accesses(const Cfg& cfg, const Function& f) {
    std::map<uint32_t, size_t> index;
    for (size_t i = 0; i < f.insns.size(); ++i) index[f.insns[i]] = i;
    const size_t n = f.insns.size();
    std::vector<std::optional<Regs>> in_state(n);
    if (n) in_state[0] = Regs{};
    std::vector<size_t> work;
    if (n) work.push_back(0);
    while (!work.empty()) {
        const size_t i = work.back();
        work.pop_back();
        const uint32_t pc = f.insns[i];
        const Insn ins = cfg.insn(pc);
        Regs out = *in_state[i];
        transfer(ins, out);
        std::vector<size_t> succ;
        if ((ins.op == Op::b || ins.op == Op::bc) && !ins.lk())
            if (auto it = index.find(ins.branch_target(pc)); it != index.end()) succ.push_back(it->second);
        if (ins.op == Op::bcctr && !ins.lk())
            for (const JumpTable& jt : f.jtabs)
                if (jt.at == pc)
                    for (uint32_t t : jt.targets)
                        if (auto it = index.find(t); it != index.end()) succ.push_back(it->second);
        if (!Cfg::is_terminator(ins))
            if (auto it = index.find(pc + 4); it != index.end()) succ.push_back(it->second);
        for (size_t s : succ) {
            const Regs merged = in_state[s] ? meet(*in_state[s], out) : out;
            if (!in_state[s] || !(merged == *in_state[s])) {
                in_state[s] = merged;
                work.push_back(s);
            }
        }
    }
    std::vector<HwAccess> found;
    for (size_t i = 0; i < n; ++i) {
        if (!in_state[i]) continue;
        const Insn ins = cfg.insn(f.insns[i]);
        if (!ins.valid() || !accesses_memory(ins)) continue;
        const std::optional<uint32_t> ea = effective_address(ins, *in_state[i]);
        if (ea && !hw_region(*ea).empty()) found.push_back({f.insns[i], *ea, access_size(ins), is_store(ins)});
    }
    return found;
}

}  // namespace qilin
