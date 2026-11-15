/**
 * @file        codegen/dcbz_test.cpp
 * @brief       Tests for the code the generator writes for dcbz and dcbzl
 *
 * The Xbox 360 has no short cache line: plain dcbz clears the whole 128-byte line, like the
 * dcbz128 form (dcbzl). These tests feed the generator single instructions and look at the text it
 * writes: the address is rounded down to 128 bytes and 128 bytes are cleared, for both forms.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <rex/codegen/codegen_context.h>
#include <rex/codegen/test_support.h>

using namespace rex::codegen;

namespace {

constexpr uint32_t kBase = 0x82000000;

// Big-endian words of the instructions used below.
constexpr uint32_t kDcbzR10R7 = 0x7C0A3FEC;   // dcbz r10,r7
constexpr uint32_t kDcbzR0R11 = 0x7C005FEC;   // dcbz r0,r11
constexpr uint32_t kDcbzlR10R7 = 0x7C2A3FEC;  // dcbzl r10,r7 (dcbz128)
constexpr uint32_t kDcbzlR0R11 = 0x7C205FEC;  // dcbzl r0,r11
constexpr uint32_t kBlr = 0x4E800020;         // blr

std::string EmitFunction(const std::vector<uint32_t>& words) {
  std::vector<uint8_t> data;
  for (uint32_t w : words) {
    data.insert(data.end(), {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w)});
  }
  const uint32_t size = static_cast<uint32_t>(data.size());

  TestModule module;
  module.Load(kBase, data.data(), data.size());

  RecompilerConfig config;
  config.projectName = "testproj";
  config.outDirectoryPath = "generated";
  std::optional<CodegenContext> ctx;
  ctx.emplace(CodegenContext::Create(BinaryView::fromModule(module), std::move(config)));
  ctx->analysisState().format = "xex";
  ctx->analysisState().loadAddress = kBase;
  ctx->analysisState().entryPoint = kBase;
  ctx->analysisState().imageSize = size;

  FunctionNode* fn = ctx->graph.addFunction(kBase, size, FunctionAuthority::DISCOVERED, true);
  REQUIRE(fn != nullptr);
  fn->discover({{kBase, size}}, {}, {});
  fn->seal();

  EmitContext emitCtx{ctx->binary(), ctx->Config(), ctx->graph, kBase, nullptr};
  return fn->emitCpp(emitCtx);
}

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("dcbz clears the whole 128-byte line", "[codegen][dcbz]") {
  const std::string code = EmitFunction({kDcbzR10R7, kBlr});
  INFO(code);
  REQUIRE(Contains(code, "// dcbz r10,r7"));
  CHECK(Contains(code, "ea = (ctx.r10.u32 + ctx.r7.u32) & ~127;"));
  CHECK(Contains(code, "memset((void*)REX_RAW_ADDR(ea), 0, 128);"));
  CHECK_FALSE(Contains(code, "& ~31"));
  CHECK_FALSE(Contains(code, ", 0, 32)"));
}

TEST_CASE("dcbz with r0 as the base register uses the other register alone", "[codegen][dcbz]") {
  const std::string code = EmitFunction({kDcbzR0R11, kBlr});
  INFO(code);
  REQUIRE(Contains(code, "// dcbz r0,r11"));
  CHECK(Contains(code, "ea = (ctx.r11.u32) & ~127;"));
  CHECK(Contains(code, "memset((void*)REX_RAW_ADDR(ea), 0, 128);"));
}

TEST_CASE("dcbzl (dcbz128) writes the same code as dcbz", "[codegen][dcbz]") {
  const std::string plain = EmitFunction({kDcbzR10R7, kBlr});
  const std::string wide = EmitFunction({kDcbzlR10R7, kBlr});
  INFO(wide);
  REQUIRE(Contains(wide, "// dcbzl r10,r7"));
  CHECK(Contains(wide, "ea = (ctx.r10.u32 + ctx.r7.u32) & ~127;"));
  CHECK(Contains(wide, "memset((void*)REX_RAW_ADDR(ea), 0, 128);"));

  // Apart from the comment that names the instruction the two functions are the same text.
  const auto body = [](std::string code, const char* name) {
    const std::string comment = std::string("// ") + name + " r10,r7";
    const size_t at = code.find(comment);
    REQUIRE(at != std::string::npos);
    return code.erase(at, comment.size());
  };
  CHECK(body(plain, "dcbz") == body(wide, "dcbzl"));

  const std::string wideR0 = EmitFunction({kDcbzlR0R11, kBlr});
  CHECK(Contains(wideR0, "ea = (ctx.r11.u32) & ~127;"));
}
