/**
 * @file        tests/unit/codegen/dcbz_test.cpp
 * @brief       Tests for the code the generator writes for dcbz and dcbzl
 *
 * The Xbox 360 has no short cache line: plain dcbz clears the whole 128-byte line, like the
 * dcbz128 form (dcbzl). These tests generate code for single instructions and look at the text:
 * the address is rounded down to 128 bytes and 128 bytes are cleared, for both forms.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "code_image.h"
#include "rex_test.h"
#include "temp_folder.h"

#include <string>
#include <string_view>
#include <vector>

namespace {

using rex::u32;

// Big-endian words of the instructions used below.
constexpr u32 kDcbzR10R7 = 0x7C0A3FEC;   // dcbz r10,r7
constexpr u32 kDcbzR0R11 = 0x7C005FEC;   // dcbz r0,r11
constexpr u32 kDcbzlR10R7 = 0x7C2A3FEC;  // dcbzl r10,r7 (dcbz128)
constexpr u32 kDcbzlR0R11 = 0x7C205FEC;  // dcbzl r0,r11
constexpr u32 kBlr = 0x4E800020;         // blr

std::string Generate(const std::vector<u32>& words) {
  const rex::test::PDataEntry pdata[] = {
      {rex::test::kCodeImageBase + rex::test::kCodeImageCodeOffset,
       static_cast<u32>(words.size())}};
  rex::test::TempFolder folder;
  const auto result =
      rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(words, pdata), "");
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  return rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
}

bool Has(const std::string& text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

// The statements of the first dcbz or dcbzl: from the address line to the end of the memset. The
// whole tree is not compared: it holds a stamp file whose hash follows the image bytes.
std::string Statements(const std::string& text) {
  const size_t begin = text.find("ea = (");
  REQUIRE(begin != std::string::npos);
  constexpr std::string_view kEnd = "0, 128);";
  const size_t end = text.find(kEnd, begin);
  REQUIRE(end != std::string::npos);
  return text.substr(begin, end + kEnd.size() - begin);
}

}  // namespace

// CHECK: the expected lines are the ones the 0.10 builder wrote ("ea = (...) & ~127;" and the
// memset of 128 bytes). Upstream's builder (src/codegen/builder/system.cpp) writes the same text,
// but nothing here was built or run.

REX_TEST_CASE("dcbz clears the whole 128-byte line", "[codegen][dcbz]") {
  const std::string code = Generate({kDcbzR10R7, kBlr});
  INFO(code);
  REQUIRE(Has(code, "// dcbz r10,r7"));
  CHECK(Has(code, "ea = (ctx.r10.u32 + ctx.r7.u32) & ~127;"));
  CHECK(Has(code, "memset((void*)REX_RAW_ADDR(ea), 0, 128);"));
  CHECK_FALSE(Has(code, "& ~31"));
  CHECK_FALSE(Has(code, ", 0, 32)"));
}

REX_TEST_CASE("dcbz with r0 as the base register uses the other register alone",
              "[codegen][dcbz]") {
  const std::string code = Generate({kDcbzR0R11, kBlr});
  INFO(code);
  REQUIRE(Has(code, "// dcbz r0,r11"));
  CHECK(Has(code, "ea = (ctx.r11.u32) & ~127;"));
  CHECK(Has(code, "memset((void*)REX_RAW_ADDR(ea), 0, 128);"));
}

REX_TEST_CASE("dcbzl (dcbz128) writes the same code as dcbz", "[codegen][dcbz]") {
  const std::string plain = Generate({kDcbzR10R7, kBlr});
  const std::string wide = Generate({kDcbzlR10R7, kBlr});
  INFO(wide);
  REQUIRE(Has(wide, "// dcbzl r10,r7"));
  CHECK(Has(wide, "ea = (ctx.r10.u32 + ctx.r7.u32) & ~127;"));
  CHECK(Has(wide, "memset((void*)REX_RAW_ADDR(ea), 0, 128);"));

  // Apart from the comment that names the instruction the two functions write the same text.
  CHECK(Statements(plain) == Statements(wide));

  const std::string wide_r0 = Generate({kDcbzlR0R11, kBlr});
  CHECK(Has(wide_r0, "ea = (ctx.r11.u32) & ~127;"));
}
