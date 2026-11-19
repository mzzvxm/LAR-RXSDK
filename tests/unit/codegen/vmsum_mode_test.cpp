/**
 * @file        tests/unit/codegen/vmsum_mode_test.cpp
 * @brief       Tests for the code the generator writes for vmsum3fp128 and vmsum4fp128
 *
 * Each of the two dot products has a fast and an exact form, chosen by the [vector_math] keys
 * vmsum3fp128 and vmsum4fp128 (fast is the default):
 *
 *   - exact: rex::ppc::simd::DotProduct3 / DotProduct4, the bit exact model of the console.
 *   - fast: rex::ppc::Vmsum3fp128 / Vmsum4fp128 (include/rex/ppc/dot_product.h), the host dot
 *     product plus the check that gives the console's QNaN when finite inputs overflow.
 *
 * Neither form may emit the host's dot product instruction (simde_mm_dp_ps) at the site: the fast
 * one reaches it only through the helper, whose code lives in the header. The values the two forms
 * give are tested by the PPC asm tests (instr_vmsum3fp128.s, instr_vmsum4fp128.s) and
 * dot_product_overflow_test.cpp; this file tests which one each key selects.
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

constexpr u32 kBlr = 0x4E800020;
constexpr u32 kVmsum3V5V3V4 = 0x14A32190;  // vmsum3fp128 v5,v3,v4
constexpr u32 kVmsum4V6V1V2 = 0x14C111D0;  // vmsum4fp128 v6,v1,v2

constexpr u32 Address(u32 index) {
  return rex::test::kCodeImageBase + rex::test::kCodeImageCodeOffset + index * 4;
}

// One function with both dot products, so that a key chosen for one cannot be taken for the other.
std::string Generate(std::string_view manifest = {}) {
  const std::vector<u32> words = {kVmsum3V5V3V4, kVmsum4V6V1V2, kBlr};
  const rex::test::PDataEntry pdata[] = {{Address(0), static_cast<u32>(words.size())}};
  rex::test::TempFolder folder;
  const auto result =
      rex::test::GenerateFromImage(folder, rex::test::BuildCodeImage(words, pdata), manifest);
  INFO((result ? std::string() : result.error().message));
  REQUIRE(result);
  return rex::test::ReadGeneratedTree(folder.path / "generated" / "entry");
}

bool Has(const std::string& tree, std::string_view text) {
  return tree.find(text) != std::string::npos;
}

size_t Count(const std::string& text, std::string_view needle) {
  size_t count = 0;
  for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
    ++count;
  }
  return count;
}

// The settings LARecomp ships: every dot product exact.
constexpr std::string_view kExactBoth =
    "[entrypoint.vector_math]\nvmsum3fp128 = \"exact\"\nvmsum4fp128 = \"exact\"\n";

constexpr std::string_view kDotProduct = "simde_mm_dp_ps";
constexpr std::string_view kExact3 = "rex::ppc::simd::DotProduct3(";
constexpr std::string_view kExact4 = "rex::ppc::simd::DotProduct4(";
constexpr std::string_view kFast3 = "rex::ppc::Vmsum3fp128(";
constexpr std::string_view kFast4 = "rex::ppc::Vmsum4fp128(";

}  // namespace

REX_TEST_CASE("Both instructions are decoded as the dot products", "[codegen][vector][vmsum]") {
  const auto tree = Generate();
  CHECK(Has(tree, "// vmsum3fp128 v5,v3,v4"));
  CHECK(Has(tree, "// vmsum4fp128 v6,v1,v2"));
}

REX_TEST_CASE("With the keys LARecomp sets the dot products are the exact model",
              "[codegen][vector][vmsum]") {
  const auto tree = Generate(kExactBoth);
  CHECK(Count(tree, kExact3) == 1);
  CHECK(Count(tree, kExact4) == 1);
  CHECK_FALSE(Has(tree, kDotProduct));
  CHECK_FALSE(Has(tree, kFast3));
  CHECK_FALSE(Has(tree, kFast4));
  // The model takes the vector status register's non-Java mode bit to flush denormals.
  CHECK(Count(tree, "ctx.vscr.nj)") >= 2);
}

REX_TEST_CASE("By default the dot products are the fast helpers", "[codegen][vector][vmsum]") {
  const auto tree = Generate();
  CHECK(Count(tree, kFast3) == 1);
  CHECK(Count(tree, kFast4) == 1);
  CHECK_FALSE(Has(tree, kDotProduct));
  CHECK_FALSE(Has(tree, kExact3));
  CHECK_FALSE(Has(tree, kExact4));
}

REX_TEST_CASE("Fast set by name is the default", "[codegen][vector][vmsum]") {
  const auto tree = Generate(
      "[entrypoint.vector_math]\nvmsum3fp128 = \"fast\"\nvmsum4fp128 = \"fast\"\n");
  CHECK(Count(tree, kFast3) == 1);
  CHECK(Count(tree, kFast4) == 1);
  CHECK_FALSE(Has(tree, kDotProduct));
}

REX_TEST_CASE("Each key selects the form of its own instruction", "[codegen][vector][vmsum]") {
  const auto three_exact = Generate("[entrypoint.vector_math]\nvmsum3fp128 = \"exact\"\n");
  CHECK(Count(three_exact, kExact3) == 1);
  CHECK(Count(three_exact, kFast4) == 1);
  CHECK_FALSE(Has(three_exact, kExact4));
  CHECK_FALSE(Has(three_exact, kFast3));
  CHECK_FALSE(Has(three_exact, kDotProduct));

  const auto four_exact = Generate("[entrypoint.vector_math]\nvmsum4fp128 = \"exact\"\n");
  CHECK(Count(four_exact, kExact4) == 1);
  CHECK(Count(four_exact, kFast3) == 1);
  CHECK_FALSE(Has(four_exact, kExact3));
  CHECK_FALSE(Has(four_exact, kFast4));
  CHECK_FALSE(Has(four_exact, kDotProduct));
}

REX_TEST_CASE("The other vector math keys do not change the dot products",
              "[codegen][vector][vmsum]") {
  const auto tree = Generate(
      "[entrypoint.vector_math]\nvmaddfp = \"exact\"\nvnmsubfp = \"exact\"\nvrefp = \"exact\"\n"
      "vrsqrtefp = \"exact\"\nvexptefp = \"exact\"\nfres = \"exact\"\nfrsqrte = \"exact\"\n");
  CHECK(Count(tree, kFast3) == 1);
  CHECK(Count(tree, kFast4) == 1);
  CHECK_FALSE(Has(tree, kExact3));
  CHECK_FALSE(Has(tree, kExact4));
  CHECK_FALSE(Has(tree, kDotProduct));
}
