/**
 * @file        tests/unit/codegen/gpr_save_runtime_test.cpp
 * @brief       Runs the GPR and LR save and restore helpers of the generated pch.h
 *
 * The code generator expands a call to the compiler's __savegprlr_N and __restgprlr_N stubs
 * (N = 14 to 31) into rex_savegprlr<N> and rex_restgprlr<N>, and keeps the stub as the fallback
 * for a stack pointer whose save area is not one flat host range. millicode_test.cpp checks the
 * text of that emission; this file runs the helpers.
 *
 * The helpers are not copied here. CMake cuts their text out of resources/templates/codegen/
 * pch_h.inja at configure time (see tests/unit/CMakeLists.txt), so the code under test is the code
 * a generated pch.h carries. It is included twice, as the template writes it and with
 * REX_CONFIG_SKIP_LR, the two shapes a project can generate.
 *
 * The reference is the stub itself: the instruction words the Xbox 360 compiler emits for a
 * __savegprlr_N or __restgprlr_N run, run by a small interpreter over a byte array, with the stub's
 * big endian loads and stores, its r1 relative effective addresses (a u32 sum, no physical alias
 * remap: the generated code reaches r1 through REX_STACK_ADDR) and its mtlr.
 *
 * Each case runs for every N from 14 to 31 and for stack pointers in range, near 0, around the
 * 0xE0000000 window and above it. In range the helper must leave the same registers and the same
 * stack bytes as the stub. Elsewhere it must call the fallback once, with its own arguments, and
 * touch nothing else.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <rex/ppc/context.h>
#include <rex/ppc/recompiled_code.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// The helper text of pch_h.inja, as written there (the link register follows the restore) and as
// REX_CONFIG_SKIP_LR generates it.
namespace lr_kept {
#include "gpr_save_helpers.inc"
}  // namespace lr_kept

#define REX_CONFIG_SKIP_LR
namespace lr_skipped {
#include "gpr_save_helpers.inc"
}  // namespace lr_skipped
#undef REX_CONFIG_SKIP_LR

namespace {

constexpr PPCRegister PPCContext::* kGprs[32] = {
    &PPCContext::r0,  &PPCContext::r1,  &PPCContext::r2,  &PPCContext::r3,  &PPCContext::r4,
    &PPCContext::r5,  &PPCContext::r6,  &PPCContext::r7,  &PPCContext::r8,  &PPCContext::r9,
    &PPCContext::r10, &PPCContext::r11, &PPCContext::r12, &PPCContext::r13, &PPCContext::r14,
    &PPCContext::r15, &PPCContext::r16, &PPCContext::r17, &PPCContext::r18, &PPCContext::r19,
    &PPCContext::r20, &PPCContext::r21, &PPCContext::r22, &PPCContext::r23, &PPCContext::r24,
    &PPCContext::r25, &PPCContext::r26, &PPCContext::r27, &PPCContext::r28, &PPCContext::r29,
    &PPCContext::r30, &PPCContext::r31};

constexpr unsigned kFirstFirst = 14;
constexpr unsigned kCount = 18;  // N = 14 to 31
constexpr u32 kWindowSpan = 0x200;

constexpr u32 kBlr = 0x4E800020;
constexpr u32 kMtlrR12 = 0x7D8803A6;

// A D form word: opcode, rt, ra, 16 bit displacement. std (62) and ld (58) are DS form with the
// low two bits 0, which the displacements of these stubs (multiples of 8) already are.
constexpr u32 DForm(u32 opcode, u32 rt, u32 ra, int disp) {
  return opcode << 26 | rt << 21 | ra << 16 | (static_cast<u32>(disp) & 0xFFFF);
}

// The words of __savegprlr_<first> (std r<n>,-8*(33-n)(r1) for n = first..31; stw r12,-8(r1); blr)
// and of __restgprlr_<first> (ld r<n>,...; lwz r12,-8(r1); mtlr r12; blr).
std::vector<u32> StubWords(bool save, u32 first) {
  std::vector<u32> words;
  for (u32 reg = first; reg <= 31; ++reg) {
    words.push_back(DForm(save ? 62 : 58, reg, 1, -8 * static_cast<int>(33 - reg)));
  }
  if (save) {
    words.push_back(DForm(36, 12, 1, -8));
  } else {
    words.push_back(DForm(32, 12, 1, -8));
    words.push_back(kMtlrR12);
  }
  words.push_back(kBlr);
  return words;
}

// The registers a stub can change or a helper could wrongly change.
struct Gprs {
  std::array<u64, 32> r{};
  u64 lr = 0;
  u64 ctr = 0;
  bool operator==(const Gprs&) const = default;
};

u64 Mix(u64 value) {
  value += 0x9E3779B97F4A7C15ull;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

void Seed(PPCContext& ctx, u32 sp, u64 salt) {
  for (u32 i = 0; i < 32; ++i) {
    (ctx.*kGprs[i]).u64 = Mix(salt * 64 + i);
  }
  ctx.r1.u64 = sp;
  ctx.lr = Mix(salt * 64 + 40);
  ctx.ctr.u64 = Mix(salt * 64 + 41);
}

Gprs Snapshot(PPCContext& ctx) {
  Gprs regs;
  for (u32 i = 0; i < 32; ++i) {
    regs.r[i] = (ctx.*kGprs[i]).u64;
  }
  regs.lr = ctx.lr;
  regs.ctr = ctx.ctr.u64;
  return regs;
}

// A byte array that stands in for the guest memory around one stack pointer: the guest addresses
// sp - kWindowSpan up to sp + kWindowSpan. Base() is the guest base that makes base + sp the byte
// at the middle, which is all the helpers compute.
struct Window {
  std::array<u8, 2 * kWindowSpan> bytes{};
  u32 sp = 0;

  Window(u32 stack_pointer, u64 salt) : sp(stack_pointer) {
    for (size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] = static_cast<u8>(Mix(salt * 4096 + i));
    }
  }

  u8* Base() {
    return reinterpret_cast<u8*>(reinterpret_cast<uintptr_t>(bytes.data() + kWindowSpan) -
                                 static_cast<uintptr_t>(sp));
  }

  // The byte of a guest address, or null when the address is outside the window.
  u8* At(u32 address) {
    const int distance = static_cast<int>(address - sp);
    if (distance < -static_cast<int>(kWindowSpan) || distance >= static_cast<int>(kWindowSpan)) {
      return nullptr;
    }
    return bytes.data() + kWindowSpan + distance;
  }
};

bool SameBytes(const Window& a, const Window& b) {
  return std::memcmp(a.bytes.data(), b.bytes.data(), a.bytes.size()) == 0;
}

u64 LoadBE(const u8* p, int size) {
  u64 value = 0;
  for (int i = 0; i < size; ++i) {
    value = value << 8 | p[i];
  }
  return value;
}

void StoreBE(u8* p, u64 value, int size) {
  for (int i = size - 1; i >= 0; --i) {
    p[i] = static_cast<u8>(value);
    value >>= 8;
  }
}

// Runs the stub's words. The effective address is the u32 sum of r<ra> and the displacement, as
// the generated r1 accesses compute it. Returns false on a word the stubs do not contain, or on
// an access outside the window.
bool RunStub(Gprs& regs, Window& memory, const std::vector<u32>& words, bool tracks_lr) {
  for (const u32 word : words) {
    if (word == kBlr) {
      return true;
    }
    if (word == kMtlrR12) {
      if (tracks_lr) {
        regs.lr = regs.r[12];
      }
      continue;
    }
    const u32 opcode = word >> 26;
    const u32 rt = word >> 21 & 31;
    const u32 ra = word >> 16 & 31;
    const bool ds_form = opcode == 62 || opcode == 58;
    const int disp = static_cast<int16_t>(word & (ds_form ? 0xFFFC : 0xFFFF));
    u8* p = memory.At(static_cast<u32>(regs.r[ra]) + static_cast<u32>(disp));
    if (!p) {
      return false;
    }
    switch (opcode) {
      case 62:  // std
        StoreBE(p, regs.r[rt], 8);
        break;
      case 36:  // stw
        StoreBE(p, regs.r[rt] & 0xFFFFFFFFull, 4);
        break;
      case 58:  // ld
        regs.r[rt] = LoadBE(p, 8);
        break;
      case 32:  // lwz
        regs.r[rt] = LoadBE(p, 4);
        break;
      default:
        return false;
    }
  }
  return false;  // no blr
}

// Independent of the helper's own compare: every access of the run starts at or above 0 and
// below 0xE0000000, so the save area is one flat host range. The lowest access is r<first> at
// sp - 8 * (33 - first), the highest starts at sp - 8.
bool ExpectFlat(u32 first, u32 sp) {
  const u64 bytes = 8ull * (33 - first);
  return u64{sp} >= bytes && u64{sp} - 8 < 0xE0000000ull;
}

// What the generated code calls when the save area is not flat. Here it only records the call.
struct FallbackLog {
  int calls = 0;
  PPCContext* ctx = nullptr;
  u8* base = nullptr;
};
FallbackLog g_fallback;

void RecordingStub(PPCContext& ctx, u8* base) {
  ++g_fallback.calls;
  g_fallback.ctx = &ctx;
  g_fallback.base = base;
}

struct KeepLr {
  static constexpr bool kTracksLr = true;
  static constexpr const char* kName = "lr kept";
  template <unsigned First>
  static void Save(PPCContext& ctx, u8* base) {
    lr_kept::rex_savegprlr<First>(ctx, base, &RecordingStub);
  }
  template <unsigned First>
  static void Restore(PPCContext& ctx, u8* base) {
    lr_kept::rex_restgprlr<First>(ctx, base, &RecordingStub);
  }
  template <unsigned First>
  static bool Flat(u32 sp) {
    return lr_kept::rex_gprlr_area_is_flat<First>(sp);
  }
};

struct SkipLr {
  static constexpr bool kTracksLr = false;
  static constexpr const char* kName = "skip lr";
  template <unsigned First>
  static void Save(PPCContext& ctx, u8* base) {
    lr_skipped::rex_savegprlr<First>(ctx, base, &RecordingStub);
  }
  template <unsigned First>
  static void Restore(PPCContext& ctx, u8* base) {
    lr_skipped::rex_restgprlr<First>(ctx, base, &RecordingStub);
  }
  template <unsigned First>
  static bool Flat(u32 sp) {
    return lr_skipped::rex_gprlr_area_is_flat<First>(sp);
  }
};

// Calls fn(std::integral_constant<unsigned, N>) for N = 14 to 31.
template <typename Fn>
void ForEachFirst(Fn&& fn) {
  [&]<unsigned... I>(std::integer_sequence<unsigned, I...>) {
    (fn(std::integral_constant<unsigned, kFirstFirst + I>{}), ...);
  }(std::make_integer_sequence<unsigned, kCount>{});
}

// The stack pointers of one N: a typical guest stack, the lowest sp that does not wrap, the ones
// just under it and near 0, the top of the flat range and the first sp above it, the physical
// window and the top of the address space.
std::vector<u32> StackPointers(u32 first) {
  const u32 bytes = 8 * (33 - first);
  return {0,          8,          0x10,       bytes - 8,  bytes - 1,  bytes,      bytes + 8,
          0x1000,     0x70000000, 0x7FFFFF00, 0x80000000, 0xDFFFFF00, 0xDFFFFFF8, 0xE0000000,
          0xE0000007, 0xE0000008, 0xE0000010, 0xE0001000, 0xF0000000, 0xFFFFFFF0, 0xFFFFFFF8,
          0xFFFFFFFF};
}

template <typename Policy, unsigned First>
void CheckSave(u32 sp, u64 salt) {
  INFO(std::format("{}: save r{} to r31, sp {:#010x}", Policy::kName, First, sp));
  PPCContext ctx{};
  Seed(ctx, sp, salt);
  const Gprs before = Snapshot(ctx);
  Window actual(sp, salt);
  const Window initial = actual;
  g_fallback = {};

  Policy::template Save<First>(ctx, actual.Base());

  if (ExpectFlat(First, sp)) {
    Window expected = initial;
    Gprs regs = before;
    REQUIRE(RunStub(regs, expected, StubWords(true, First), Policy::kTracksLr));
    CHECK(g_fallback.calls == 0);
    CHECK(SameBytes(actual, expected));
    CHECK(Snapshot(ctx) == regs);
  } else {
    CHECK(g_fallback.calls == 1);
    CHECK(g_fallback.ctx == &ctx);
    CHECK(g_fallback.base == actual.Base());
    CHECK(SameBytes(actual, initial));
    CHECK(Snapshot(ctx) == before);
  }
}

template <typename Policy, unsigned First>
void CheckRestore(u32 sp, u64 salt) {
  INFO(std::format("{}: restore r{} to r31, sp {:#010x}", Policy::kName, First, sp));
  PPCContext ctx{};
  Seed(ctx, sp, salt);
  const Gprs before = Snapshot(ctx);
  Window actual(sp, salt + 1000);
  const Window initial = actual;
  g_fallback = {};

  Policy::template Restore<First>(ctx, actual.Base());

  if (ExpectFlat(First, sp)) {
    Window expected = initial;
    Gprs regs = before;
    REQUIRE(RunStub(regs, expected, StubWords(false, First), Policy::kTracksLr));
    CHECK(g_fallback.calls == 0);
    CHECK(SameBytes(actual, expected));  // a restore writes no memory
    CHECK(Snapshot(ctx) == regs);
  } else {
    CHECK(g_fallback.calls == 1);
    CHECK(g_fallback.ctx == &ctx);
    CHECK(g_fallback.base == actual.Base());
    CHECK(SameBytes(actual, initial));
    CHECK(Snapshot(ctx) == before);
  }
}

template <typename Policy>
void SweepSaveAndRestore() {
  u64 salt = 1;
  ForEachFirst([&](auto first) {
    constexpr unsigned kFirst = decltype(first)::value;
    for (const u32 sp : StackPointers(kFirst)) {
      CheckSave<Policy, kFirst>(sp, salt++);
      CheckRestore<Policy, kFirst>(sp, salt++);
    }
  });
}

}  // namespace

REX_TEST_CASE("The stub words are the compiler's", "[codegen][millicode][runtime]") {
  // The words of the Xbox 360 compiler's stubs, so that the interpreter below runs the real thing.
  const auto save = StubWords(true, 14);
  REQUIRE(save.size() == 18 + 2);
  CHECK(save.front() == 0xF9C1FF68);  // std r14,-0x98(r1)
  CHECK(save[17] == 0xFBE1FFF0);      // std r31,-0x10(r1)
  CHECK(save[18] == 0x9181FFF8);      // stw r12,-8(r1)
  CHECK(save[19] == kBlr);

  const auto restore = StubWords(false, 14);
  REQUIRE(restore.size() == 18 + 3);
  CHECK(restore.front() == 0xE9C1FF68);  // ld r14,-0x98(r1)
  CHECK(restore[17] == 0xEBE1FFF0);      // ld r31,-0x10(r1)
  CHECK(restore[18] == 0x8181FFF8);      // lwz r12,-8(r1)
  CHECK(restore[19] == kMtlrR12);
  CHECK(restore[20] == kBlr);

  const auto last = StubWords(true, 31);
  REQUIRE(last.size() == 1 + 2);
  CHECK(last.front() == 0xFBE1FFF0);
}

REX_TEST_CASE("The save area is r14 at sp-0x98 up to r31 at sp-0x10, and the link word at sp-8",
              "[codegen][millicode][runtime]") {
  // A fixed layout, written out by hand, so that the helper is not only compared with a model
  // built from the same formula.
  PPCContext ctx{};
  const u32 sp = 0x70001230;
  Seed(ctx, sp, 7);
  ctx.r12.u64 = 0xFFFFFFFF88C0FFEEull;
  Window window(sp, 7);
  std::memset(window.bytes.data(), 0xCC, window.bytes.size());
  g_fallback = {};

  lr_kept::rex_savegprlr<14>(ctx, window.Base(), &RecordingStub);

  CHECK(LoadBE(window.At(sp - 0x98), 8) == ctx.r14.u64);
  CHECK(LoadBE(window.At(sp - 0x90), 8) == ctx.r15.u64);
  CHECK(LoadBE(window.At(sp - 0x10), 8) == ctx.r31.u64);
  // Only the low word of r12 is stored, and the bytes above the area are not touched.
  CHECK(LoadBE(window.At(sp - 8), 4) == 0x88C0FFEEull);
  CHECK(LoadBE(window.At(sp - 4), 4) == 0xCCCCCCCCull);
  CHECK(LoadBE(window.At(sp - 0x99), 1) == 0xCCull);
  CHECK(window.At(sp - 0x98)[0] == static_cast<u8>(ctx.r14.u64 >> 56));  // big endian
  CHECK(window.At(sp - 0x98)[7] == static_cast<u8>(ctx.r14.u64));
  CHECK(g_fallback.calls == 0);
}

REX_TEST_CASE("The helpers match the stubs with the link register kept",
              "[codegen][millicode][runtime]") {
  SweepSaveAndRestore<KeepLr>();
}

REX_TEST_CASE("The helpers match the stubs when the project skips the link register",
              "[codegen][millicode][runtime]") {
  SweepSaveAndRestore<SkipLr>();
}

REX_TEST_CASE("A restore sets r12 to the stored word, zero extended, and lr to it",
              "[codegen][millicode][runtime]") {
  PPCContext ctx{};
  const u32 sp = 0x7FFFF000;
  Seed(ctx, sp, 3);
  ctx.r12.u64 = 0xFFFFFFFFFFFFFFFFull;
  ctx.lr = 0xFFFFFFFFFFFFFFFFull;
  Window window(sp, 3);
  StoreBE(window.At(sp - 8), 0x82345678, 4);
  g_fallback = {};

  lr_kept::rex_restgprlr<20>(ctx, window.Base(), &RecordingStub);

  CHECK(ctx.r12.u64 == 0x82345678ull);
  CHECK(ctx.lr == 0x82345678ull);
  CHECK(g_fallback.calls == 0);
}

REX_TEST_CASE("A save followed by a restore gives the registers back", "[codegen][millicode][runtime]") {
  ForEachFirst([&](auto first) {
    constexpr unsigned kFirst = decltype(first)::value;
    PPCContext ctx{};
    const u32 sp = 0x70FFE000;
    Seed(ctx, sp, kFirst);
    ctx.r12.u64 = 0x82ABCDEF;  // a code address, the value mflr leaves in r12
    const Gprs original = Snapshot(ctx);
    Window window(sp, kFirst);
    g_fallback = {};

    lr_kept::rex_savegprlr<kFirst>(ctx, window.Base(), &RecordingStub);
    // The callee clobbers the non volatile registers it saved, and r12.
    for (unsigned n = kFirst; n <= 31; ++n) {
      (ctx.*kGprs[n]).u64 = ~(ctx.*kGprs[n]).u64;
    }
    ctx.r12.u64 = 0;
    lr_kept::rex_restgprlr<kFirst>(ctx, window.Base(), &RecordingStub);

    INFO(std::format("r{} to r31", kFirst));
    CHECK(Snapshot(ctx).r == original.r);
    CHECK(ctx.lr == 0x82ABCDEFull);
    CHECK(g_fallback.calls == 0);
  });
}

REX_TEST_CASE("The flat test agrees with the stub's range for every N",
              "[codegen][millicode][runtime]") {
  // The edges of every N, then a spread of stack pointers over the whole 32 bit space.
  ForEachFirst([&](auto first) {
    constexpr unsigned kFirst = decltype(first)::value;
    std::vector<u32> values = StackPointers(kFirst);
    const u32 bytes = 8 * (33 - kFirst);
    for (const u32 edge : {bytes, 0xE0000000u, 0xE0000008u}) {
      for (u32 delta = 0; delta < 24; ++delta) {
        values.push_back(edge - 12 + delta);
      }
    }
    for (u64 i = 0; i < 4000; ++i) {
      values.push_back(static_cast<u32>(Mix(i * 100 + kFirst)));
    }
    size_t mismatches_kept = 0;
    size_t mismatches_skipped = 0;
    for (const u32 sp : values) {
      mismatches_kept += KeepLr::Flat<kFirst>(sp) != ExpectFlat(kFirst, sp);
      mismatches_skipped += SkipLr::Flat<kFirst>(sp) != ExpectFlat(kFirst, sp);
    }
    INFO(std::format("r{} to r31", kFirst));
    CHECK(mismatches_kept == 0);
    CHECK(mismatches_skipped == 0);
  });
}

REX_TEST_CASE("The range edges of the lowest and the highest N", "[codegen][millicode][runtime]") {
  // N = 14 needs 152 bytes below sp, N = 31 needs 16; the top of the flat range is the same.
  CHECK_FALSE(KeepLr::Flat<14>(0x97));
  CHECK(KeepLr::Flat<14>(0x98));
  CHECK_FALSE(KeepLr::Flat<31>(0xF));
  CHECK(KeepLr::Flat<31>(0x10));
  CHECK(KeepLr::Flat<14>(0xE0000000));
  CHECK(KeepLr::Flat<31>(0xE0000000));
  CHECK_FALSE(KeepLr::Flat<14>(0xE0000008));
  CHECK_FALSE(KeepLr::Flat<31>(0xE0000008));
  CHECK_FALSE(KeepLr::Flat<14>(0xFFFFFFF0));
  CHECK_FALSE(KeepLr::Flat<14>(0));
}
