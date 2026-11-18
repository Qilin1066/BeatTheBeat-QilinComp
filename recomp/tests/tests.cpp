// Self-contained tests: SHA-256, a synthetic DOL built in memory, and the
// Broadway decoder. No game files required (runs in CI).
#include <algorithm>
#include <cstring>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "decoder.hpp"
#include "dol.hpp"
#include "sha256.hpp"

using namespace qilin;

namespace {

int failures = 0;
#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::println(stderr, "{}:{}: CHECK failed: {}", __FILE__, __LINE__, #cond); \
            ++failures;                                                                 \
        }                                                                               \
    } while (0)

void put_be32(std::vector<uint8_t>& d, size_t o, uint32_t v) {
    d[o] = static_cast<uint8_t>(v >> 24);
    d[o + 1] = static_cast<uint8_t>(v >> 16);
    d[o + 2] = static_cast<uint8_t>(v >> 8);
    d[o + 3] = static_cast<uint8_t>(v);
}

// ---------------------------------------------------------------- SHA-256

std::vector<uint8_t> pattern(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i * 7 + 3);
    return v;
}

void test_sha256() {
    CHECK(sha256_hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string_view abc = "abc";
    CHECK(sha256_hex({reinterpret_cast<const uint8_t*>(abc.data()), abc.size()}) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // Padding edges: the length fits in the last block, just doesn't, and exact blocks.
    CHECK(sha256_hex(pattern(55)) == "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b");
    CHECK(sha256_hex(pattern(56)) == "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27");
    CHECK(sha256_hex(pattern(63)) == "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055");
    CHECK(sha256_hex(pattern(64)) == "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241");
    CHECK(sha256_hex(pattern(119)) == "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e");
    CHECK(sha256_hex(pattern(1000)) == "1e9bc38cbf860b9ec31918b065f9b52476c549a782e0e7990bed8ce3868d2371");
    const std::vector<uint8_t> million(1000000, 'a');
    CHECK(sha256_hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// ---------------------------------------------------------------- DOL

// A synthetic DOL: code at 0x80003100, data at 0x80005000, a small-data
// section inside the BSS range (as Wii linkers lay them out), BSS 0x80006000-0x80007000.
struct SyntheticDol {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x400, 0);

    SyntheticDol() {
        section(0, 0x100, 0x80003100, 0x100);   // text0
        section(7, 0x200, 0x80005000, 0x100);   // data0
        section(8, 0x300, 0x80006800, 0x40);    // data1, inside BSS
        put_be32(bytes, 0xD8, 0x80006000);      // BSS
        put_be32(bytes, 0xDC, 0x1000);
        put_be32(bytes, 0xE0, 0x80003100);      // entry
        for (size_t i = 0x100; i < 0x200; i += 4) put_be32(bytes, i, 0x60000000);  // ori r0,r0,0
        put_be32(bytes, 0x100, 0x38600001);     // addi r3,0,1
        for (size_t i = 0x200; i < 0x340; ++i) bytes[i] = static_cast<uint8_t>(i);  // data0, data1
    }
    void section(unsigned slot, uint32_t offset, uint32_t address, uint32_t size) {
        put_be32(bytes, 4 * slot, offset);
        put_be32(bytes, 0x48 + 4 * slot, address);
        put_be32(bytes, 0x90 + 4 * slot, size);
    }
};

void test_dol() {
    SyntheticDol s;
    auto dol = Dol::parse(s.bytes);
    CHECK(dol.has_value());
    if (!dol) {
        std::println(stderr, "  {}", dol.error());
        return;
    }
    CHECK(dol->sections.size() == 3);
    CHECK(dol->sections[0].text && dol->sections[0].index == 0 && dol->sections[0].name() == "text0");
    CHECK(!dol->sections[1].text && dol->sections[1].name() == "data0");
    CHECK(dol->sections[2].name() == "data1" && dol->sections[2].address == 0x80006800);
    CHECK(dol->entry == 0x80003100);
    CHECK(dol->bss_address == 0x80006000 && dol->bss_size == 0x1000);
    CHECK(dol->base() == 0x80003100 && dol->image_end() == 0x80007000);
    CHECK(dol->u32(0x80003100) == 0x38600001);
    CHECK(dol->u32(0x80003104) == 0x60000000);
    CHECK(dol->u8(0x80005001) == 0x01);       // data0's second byte, file offset 0x201
    CHECK(dol->u8(0x80006000) == 0);           // BSS
    CHECK(dol->u8(0x80006801) == 0x01);       // data1 inside BSS keeps its bytes (offset 0x301)
    CHECK(dol->u8(0x80006840) == 0);           // BSS after data1
    CHECK(dol->u8(0x80004000) == 0);           // gap between sections
    CHECK(dol->mapped(0x80006FFC, 4) && !dol->mapped(0x80006FFD, 4) && !dol->mapped(0x800030FF));
    CHECK(dol->section_at(0x800031FC) == &dol->sections[0]);
    CHECK(dol->section_at(0x80003200) == nullptr);
    CHECK(dol->section_at(0x80005050) == &dol->sections[1]);
    CHECK(dol->text_section_at(0x80005050) == nullptr);
    CHECK(dol->text_section_at(0x80003100) != nullptr);
    CHECK(dol->file_size == 0x400);
    CHECK(dol->sha256 == sha256_hex(s.bytes));
    CHECK(known_build(dol->sha256) == nullptr);
    CHECK(known_build("2e0368359106a2eef773c70ef083e89aafe1e49603eefc7f16b9ed414cd098fe") != nullptr);
    bool threw = false;
    try {
        (void)dol->u32(0x80007000);
    } catch (const DolError&) {
        threw = true;
    }
    CHECK(threw);
    const std::string json = to_json(*dol);
    CHECK(json.contains("\"entry\": \"0x80003100\""));
    CHECK(json.contains("\"name\": \"data1\""));
    CHECK(json.contains("\"build\": null"));

    // Malformed files are error messages, not crashes.
    auto expect_error = [](std::vector<uint8_t> bytes, std::string_view what) {
        auto r = Dol::parse(bytes);
        CHECK(!r.has_value());
        if (!r) {
            if (!r.error().contains(what)) std::println(stderr, "  unexpected error: {}", r.error());
            CHECK(r.error().contains(what));
        }
    };
    expect_error(std::vector<uint8_t>(0x80, 0), "shorter than");
    expect_error(std::vector<uint8_t>(0x100, 0), "no sections");
    {
        SyntheticDol b;
        put_be32(b.bytes, 0x90, 0x400);  // text0 runs past the end of the file
        expect_error(b.bytes, "outside the file");
    }
    {
        SyntheticDol b;
        put_be32(b.bytes, 0x48, 0x7FFFFF00);  // text0 below MEM1
        expect_error(b.bytes, "outside MEM1");
    }
    {
        SyntheticDol b;
        put_be32(b.bytes, 0x48, 0x817FFFC0);  // text0 runs past the end of MEM1
        expect_error(b.bytes, "outside MEM1");
    }
    {
        SyntheticDol b;
        put_be32(b.bytes, 0x48 + 4 * 7, 0x80003180);  // data0 overlaps text0
        expect_error(b.bytes, "overlap");
    }
    {
        SyntheticDol b;
        put_be32(b.bytes, 0x48, 0x80003102);  // misaligned code
        expect_error(b.bytes, "word-aligned");
    }
    {
        SyntheticDol b;
        put_be32(b.bytes, 0xE0, 0x80005000);  // entry in data
        expect_error(b.bytes, "entry point");
    }
    {
        SyntheticDol b;
        put_be32(b.bytes, 0xD8, 0x81800000);  // BSS past MEM1
        expect_error(b.bytes, "BSS");
    }
}

// ---------------------------------------------------------------- decoder

struct Case {
    uint32_t pc, word;
    std::string_view text;
};

// Expected text from GNU objdump 2.42 `-M broadway,raw` (mftb excepted: see
// tools/check/verify_decoder.py). The sweep in CI checks millions more.
constexpr Case kCases[] = {
    {0x80003100, 0x38610008, "addi r3,r1,8"},
    {0x80003104, 0x38600000, "addi r3,0,0"},
    {0x80003108, 0x3C608000, "addis r3,0,-32768"},
    {0x8000310C, 0x3863FFFC, "addi r3,r3,-4"},
    {0x80003110, 0x80010014, "lwz r0,20(r1)"},
    {0x80003114, 0x8061FFF8, "lwz r3,-8(r1)"},
    {0x80003118, 0x9421FFE0, "stwu r1,-32(r1)"},
    {0x8000311C, 0x7C0802A6, "mfspr r0,8"},
    {0x80003120, 0x7C0803A6, "mtspr 8,r0"},
    {0x80003124, 0x7C6C42E6, "mftb r3,268"},
    {0x80003128, 0x4E800020, "bclr 20,lt,0"},
    {0x8000312C, 0x4E800421, "bcctrl 20,lt,0"},
    {0x80003130, 0x48000010, "b 0x80003140"},
    {0x80003134, 0x4BFFFFF1, "bl 0x80003124"},
    {0x80003138, 0x48000102, "ba 0x100"},
    {0x8000313C, 0x4182FFF0, "bc 12,eq,0x8000312c"},
    {0x80003140, 0x409E000C, "bc 4,4*cr7+eq,0x8000314c"},
    {0x80003144, 0x5483103A, "rlwinm r3,r4,2,0,29"},
    {0x80003148, 0x7C632214, "add r3,r3,r4"},
    {0x8000314C, 0x7C632615, "addo. r3,r3,r4"},
    {0x80003150, 0x7C032000, "cmp cr0,0,r3,r4"},
    {0x80003154, 0x2C030000, "cmpi cr0,0,r3,0"},
    {0x80003158, 0x2807FFFF, "cmpli cr0,0,r7,65535"},
    {0x8000315C, 0x4C063342, "crorc lt,4*cr1+eq,4*cr1+eq"},
    {0x80003160, 0x4C840000, "mcrf cr1,cr1"},
    {0x80003164, 0xFC20101E, "fctiwz f1,f2"},
    {0x80003168, 0xEC22182A, "fadds f1,f2,f3"},
    {0x8000316C, 0xFC2208FA, "fmadd f1,f2,f3,f1"},
    {0x80003170, 0xEC201830, "fres f1,f3,0"},
    {0x80003174, 0xFF80210C, "mtfsfi 7,2"},
    {0x80003178, 0xFDFE0D8E, "mtfsf 255,f1"},
    {0x8000317C, 0xFC20048F, "mffs. f1"},
    {0x80003180, 0xE0210008, "psq_l f1,8(r1),0,0"},
    {0x80003184, 0xF0A3FFF8, "psq_st f5,-8(r3),1,7"},
    {0x80003188, 0xE4C1C014, "psq_lu f6,20(r1),1,4"},
    {0x8000318C, 0x1062000C, "psq_lx f3,r2,r0,0,0"},
    {0x80003190, 0x1040208E, "psq_stx f2,r0,r4,0,1"},
    {0x80003194, 0x10821814, "ps_sum0 f4,f2,f0,f3"},
    {0x80003198, 0x102310FA, "ps_madd f1,f3,f3,f2"},
    {0x8000319C, 0x10642420, "ps_merge00 f3,f4,f4"},
    {0x800031A0, 0x10011040, "ps_cmpo0 cr0,f1,f2"},
    {0x800031A4, 0x100307EC, "dcbz_l r3,r0"},
    {0x800031A8, 0x7C6004AA, "lswi r3,0,32"},
    {0x800031AC, 0x44000002, "sc 0"},
    {0x800031B0, 0x7C0004AC, "sync 0"},
    {0x800031B4, 0x4C00012C, "isync"},
    {0x800031B8, 0x7C0006AC, "eieio"},
    {0x800031BC, 0x4C000064, "rfi"},
    {0x800031C0, 0x7C6FF120, "mtcrf 255,r3"},
    {0x800031C4, 0x7C64292D, "stwcx. r3,r4,r5"},
    {0x800031C8, 0x7C642828, "lwarx r3,r4,r5,0"},
    {0x800031CC, 0x7C002264, "tlbie r4,0"},
    {0x800031D0, 0x7C0018AC, "dcbf 0,r3,0"},
    {0x800031D4, 0x0C9F0000, "twi 4,r31,0"},
    {0x800031D8, 0x7FE00008, "tw 31,r0,r0"},
    {0x800031DC, 0x7C631670, "srawi r3,r3,2"},
    {0x800031E0, 0x7C640734, "extsh r4,r3"},
    {0x800031E4, 0x7C6003A6, "mtspr 0,r3"},
    {0x800031E8, 0xBBC1FFF8, "lmw r30,-8(r1)"},
    {0x800031EC, 0x7C8004A6, "mfsr r4,0"},
    {0x800031F0, 0x7C8401A4, "mtsr 4,r4"},
    {0x800031F4, 0x57E3063E, "rlwinm r3,r31,0,24,31"},
    {0x800031F8, 0x7D435378, "or r3,r10,r10"},
};

// Words that must not decode, and why.
struct Bad {
    uint32_t word;
    std::string_view why;
};
constexpr Bad kInvalid[] = {
    {0x00000000, "primary opcode 0"},
    {0x84630000, "lwzu r3,0(r3): base is the target"},
    {0x84600000, "lwzu r3,0(0): no base"},
    {0x94600000, "stwu r3,0(0): update with rA=0"},
    {0x40E00000, "bc with BO 0b00111: a z bit set"},
    {0x4E000420, "bcctr with BO 0b10000: decrements CTR"},
    {0x2C230000, "cmpi with L=1"},
    {0xFC20102C, "fsqrt: not on the 750CL"},
    {0xE4600000, "psq_lu with rA=0"},
    {0x7C641C2A, "lswx r3,r4,r3: rD = rB"},
    {0x7C83202F, "lwzx with the record bit set"},
    {0x7C632416, "mulhwu with bit 21 (OE) set"},
    {0x7C6304AA, "lswi r3,r3,32: rA among the registers it loads"},
    {0xBB9E0000, "lmw r28,0(r30): base among the loaded registers"},
    {0x44000003, "sc with bit 31 set"},
    {0x7C000265, "tlbie with the record bit set"},
    {0x7C4004AC, "sync with bit 9 set"},
    {0x7C0002E4, "tlbia: not on the 750CL"},
};

// One valid word per instruction: the opcode bits plus operand fields taken
// from a template chosen to satisfy every operand constraint (rD/BO/TO = 20,
// rA = 4, rB/NB = 5, frC = 6).
uint32_t canonical(const ppc::Spec& s) {
    constexpr uint32_t kTemplate = (20u << 21) | (4u << 16) | (5u << 11) | (6u << 6) | (7u << 1);
    uint32_t fields = 0;
    for (ppc::Opnd o : s.operands) fields |= ppc::field_mask(o);
    return s.match | (kTemplate & fields & ~s.mask);
}

void test_decoder() {
    for (const Case& c : kCases) {
        const ppc::Insn in = ppc::decode(c.word);
        const std::string text = ppc::disasm(in, c.pc);
        if (text != c.text) std::println(stderr, "  {:08x}: got '{}', want '{}'", c.word, text, c.text);
        CHECK(text == c.text);
    }
    for (const Bad& b : kInvalid) {
        const bool invalid = !ppc::decode(b.word).valid();
        if (!invalid) std::println(stderr, "  {:08x} decoded as {} ({})", b.word, ppc::disasm(ppc::decode(b.word), 0), b.why);
        CHECK(invalid);
    }

    // Every instruction decodes from its canonical word, and setting any one
    // bit it doesn't define (a reserved bit) makes the word invalid.
    CHECK(ppc::specs().size() == static_cast<size_t>(ppc::Op::count_) - 1);
    size_t checked = 0;
    for (const ppc::Spec& s : ppc::specs()) {
        const uint32_t word = canonical(s);
        const ppc::Insn in = ppc::decode(word);
        if (in.op != s.op) std::println(stderr, "  {}: canonical word {:08x} decodes as {}", s.name, word, ppc::spec(in.op).name);
        CHECK(in.op == s.op);
        CHECK(ppc::spec(s.op).name == s.name);
        uint32_t defined = s.mask;
        for (ppc::Opnd o : s.operands) defined |= ppc::field_mask(o);
        if (s.flags & (ppc::kRc | ppc::kLk)) defined |= 1;
        if (s.flags & ppc::kOe) defined |= 0x400;
        if (s.flags & ppc::kAa) defined |= 2;
        for (unsigned bit = 0; bit < 32; ++bit) {
            if (defined & (1u << bit)) continue;
            const ppc::Insn flipped = ppc::decode(word ^ (1u << bit));
            if (flipped.valid())
                std::println(stderr, "  {}: reserved bit {} set still decodes ({:08x} -> {})", s.name, bit,
                             word ^ (1u << bit), ppc::disasm(flipped, 0));
            CHECK(!flipped.valid());
        }
        ++checked;
    }
    CHECK(checked == 222);  // the 750CL's instructions, paired singles included

    // Fields and suffixes.
    CHECK(ppc::decode(0x3863FFFC).simm() == -4);
    CHECK(ppc::decode(0x4BFFFFF1).branch_target(0x80003134) == 0x80003124);
    CHECK(ppc::decode(0x4BFFFFF3).branch_target(0x80003134) == 0xFFFFFFF0);  // bla -16
    CHECK(ppc::decode(0x4182FFF0).branch_target(0x8000313C) == 0x8000312C);
    CHECK(ppc::decode(0xF0A3FFF8).psq_d() == -8);
    CHECK(ppc::decode(0xF0A3FFF8).psq_w() == 1 && ppc::decode(0xF0A3FFF8).psq_i() == 7);
    CHECK(ppc::decode(0x1040208E).psqx_w() == 0 && ppc::decode(0x1040208E).psqx_i() == 1);
    CHECK(ppc::decode(0x7C6C42E6).spr() == 268);
    CHECK(ppc::mnemonic(ppc::decode(0x4BFFFFF3)) == "bla");
    CHECK(ppc::mnemonic(ppc::decode(0x7C632615)) == "addo.");
    CHECK(ppc::mnemonic(ppc::decode(0x4E800421)) == "bcctrl");
    CHECK(ppc::spec(ppc::Op::psq_l).flags & ppc::kPaired);
    CHECK(ppc::spec(ppc::Op::mtmsr).flags & ppc::kPrivileged);
    CHECK(ppc::spec(ppc::Op::bc).flags & ppc::kBranch);
    CHECK(ppc::disasm(ppc::decode(0), 0) == ".long 0x0");
}

}  // namespace

int main() {
    test_sha256();
    test_dol();
    test_decoder();
    if (failures) {
        std::println(stderr, "{} check(s) failed", failures);
        return 1;
    }
    std::println("all tests passed");
    return 0;
}
