/**
 * @file        codegen/codegen_writer_test.cpp
 * @brief       Tests for content-addressed codegen output writing
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include <rex/codegen/codegen_context.h>
#include <rex/codegen/codegen_writer.h>
#include <rex/codegen/test_support.h>

namespace fs = std::filesystem;
using namespace rex::codegen;

namespace {

constexpr uint32_t kBaseAddress = 0x82000000;
constexpr uint32_t kFunctionCount = 4;

// Big-endian `blr` (0x4E800020), one instruction per test function.
std::vector<uint8_t> MakeBlrBinary(uint32_t functionCount) {
  std::vector<uint8_t> data;
  data.reserve(functionCount * 4);
  for (uint32_t i = 0; i < functionCount; ++i) {
    data.insert(data.end(), {0x4E, 0x80, 0x00, 0x20});
  }
  return data;
}

/// Whole-file contents, for asserting on emitted text.
std::string ReadAll(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Fake module, context with a sealed function per `blr`, scratch output tree.
struct WriterFixture {
  fs::path root;
  std::vector<uint8_t> data;
  TestModule module;
  std::optional<CodegenContext> ctx;

  explicit WriterFixture(std::string_view name, uint32_t functionCount = kFunctionCount)
      : root(fs::temp_directory_path() / "rexglue_writer_test" / fs::path(name)),
        data(MakeBlrBinary(functionCount)) {
    fs::remove_all(root);
    fs::create_directories(root);

    module.Load(kBaseAddress, data.data(), data.size());

    RecompilerConfig config;
    config.projectName = "testproj";
    config.outDirectoryPath = "generated";

    ctx.emplace(CodegenContext::Create(BinaryView::fromModule(module), std::move(config)));
    ctx->setConfigDir(root);
    ctx->analysisState().format = "xex";
    ctx->analysisState().loadAddress = kBaseAddress;
    ctx->analysisState().entryPoint = kBaseAddress;
    ctx->analysisState().imageSize = static_cast<uint32_t>(data.size());

    for (uint32_t i = 0; i < functionCount; ++i) {
      uint32_t addr = kBaseAddress + i * 4;
      auto* node = ctx->graph.addFunction(addr, 4, FunctionAuthority::DISCOVERED, true);
      REQUIRE(node != nullptr);
      node->discover({{addr, 4}}, {}, {});
      node->seal();
    }
  }

  ~WriterFixture() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }

  fs::path outputDir() const { return root / "generated"; }
};

}  // namespace

TEST_CASE("Second write with unchanged inputs writes nothing", "[codegen_writer]") {
  WriterFixture fx("unchanged");

  CodegenWriter first(*fx.ctx);
  REQUIRE(first.write(false));
  REQUIRE_FALSE(first.writtenFiles().empty());

  CodegenWriter second(*fx.ctx);
  REQUIRE(second.write(false));

  CHECK(second.writtenFiles().empty());
  CHECK(second.unchangedFiles().size() == first.writtenFiles().size());
  CHECK(second.deletedFiles().empty());
}

TEST_CASE("Unchanged files keep their modification time", "[codegen_writer]") {
  WriterFixture fx("mtime");

  CodegenWriter first(*fx.ctx);
  REQUIRE(first.write(false));

  auto probe = fx.outputDir() / "testproj_init.h";
  REQUIRE(fs::exists(probe));
  auto before = fs::last_write_time(probe);

  CodegenWriter second(*fx.ctx);
  REQUIRE(second.write(false));

  // Compared as a bool: with libc++ the file clock's rep is __int128, which Catch2 cannot print
  // when it stringifies the two operands of a failed or reconstructed comparison.
  const bool same_time = fs::last_write_time(probe) == before;
  CHECK(same_time);
}

TEST_CASE("Stale generated files are swept after the write", "[codegen_writer]") {
  WriterFixture fx("stale");

  CodegenWriter first(*fx.ctx);
  REQUIRE(first.write(false));

  auto orphan = fx.outputDir() / "testproj_recomp.99.cpp";
  { std::ofstream(orphan) << "// leftover\n"; }
  REQUIRE(fs::exists(orphan));

  CodegenWriter second(*fx.ctx);
  REQUIRE(second.write(false));

  CHECK_FALSE(fs::exists(orphan));
  CHECK(std::find(second.deletedFiles().begin(), second.deletedFiles().end(),
                  std::string("testproj_recomp.99.cpp")) != second.deletedFiles().end());
}

TEST_CASE("Unrelated files in the output directory survive the sweep", "[codegen_writer]") {
  WriterFixture fx("bystander");

  CodegenWriter first(*fx.ctx);
  REQUIRE(first.write(false));

  auto bystander = fx.outputDir() / "notes.txt";
  { std::ofstream(bystander) << "keep me\n"; }

  CodegenWriter second(*fx.ctx);
  REQUIRE(second.write(false));

  CHECK(fs::exists(bystander));
}

TEST_CASE("The stamp is owned by the recompiler and survives the sweep", "[codegen_writer]") {
  WriterFixture fx("stamp");

  CodegenWriter first(*fx.ctx);
  REQUIRE(first.write(false));

  auto stamp = fx.outputDir() / "codegen.stamp";
  { std::ofstream(stamp) << "{}\n"; }

  CodegenWriter second(*fx.ctx);
  REQUIRE(second.write(false));

  CHECK(fs::exists(stamp));
  CHECK(second.deletedFiles().empty());
}

TEST_CASE("A changed function rewrites only the files that differ", "[codegen_writer]") {
  WriterFixture fx("changed");

  CodegenWriter first(*fx.ctx);
  REQUIRE(first.write(false));
  auto initialCount = first.writtenFiles().size();
  REQUIRE(initialCount > 1);

  // Touches init.h, init.cpp, register.cpp and one recomp file, not sources.cmake.
  fx.ctx->graph.setFunctionName(kBaseAddress + 4, "renamed_function");

  CodegenWriter second(*fx.ctx);
  REQUIRE(second.write(false));

  CHECK_FALSE(second.writtenFiles().empty());
  CHECK(second.writtenFiles().size() < initialCount);
  CHECK(std::find(second.writtenFiles().begin(), second.writtenFiles().end(),
                  std::string("sources.cmake")) == second.writtenFiles().end());
}

TEST_CASE("Recomp files are stable when one function's size changes", "[codegen_writer]") {
  WriterFixture fx("partition", 8);

  CodegenWriter first(*fx.ctx);
  REQUIRE(first.write(false));

  fx.ctx->graph.setFunctionName(kBaseAddress, "a_considerably_longer_function_name");

  CodegenWriter second(*fx.ctx);
  REQUIRE(second.write(false));

  size_t rewrittenRecompFiles = 0;
  for (const auto& name : second.writtenFiles()) {
    if (name.find("_recomp.") != std::string::npos)
      ++rewrittenRecompFiles;
  }
  CHECK(rewrittenRecompFiles <= 1);
  CHECK(fs::exists(fx.outputDir() / "codegen.partition.json"));
}

TEST_CASE("Every name a recomp file calls is declared in its header", "[codegen_writer]") {
  WriterFixture fx("declarations", 8);

  CodegenWriter writer(*fx.ctx);
  REQUIRE(writer.write(false));

  static const std::regex callRe(R"(\b([A-Za-z_][A-Za-z0-9_]*)\(ctx, base\))");
  static const std::regex declRe(R"(DECLARE_REX_FUNC\(([A-Za-z_][A-Za-z0-9_]*)\))");

  for (const auto& entry : fs::directory_iterator(fx.outputDir())) {
    auto name = entry.path().filename().string();
    if (name.find("_recomp.") == std::string::npos)
      continue;

    auto index = name.substr(name.find("_recomp.") + 8);
    index = index.substr(0, index.find('.'));
    auto header = fx.outputDir() / fmt::format("testproj_funcs.{}.h", index);
    REQUIRE(fs::exists(header));

    std::string body = ReadAll(entry.path());
    std::string decls = ReadAll(header);

    std::set<std::string> declared;
    for (std::sregex_iterator it(decls.begin(), decls.end(), declRe), end; it != end; ++it) {
      declared.insert((*it)[1].str());
    }

    // A call may precede its own definition, so being defined here is no excuse.
    for (std::sregex_iterator it(body.begin(), body.end(), callRe), end; it != end; ++it) {
      auto called = (*it)[1].str();
      INFO("file " << name << " calls " << called << " with no declaration");
      CHECK(declared.contains(called));
    }
  }
}

TEST_CASE("exact_e0_view is off unless the config turns it on", "[codegen_writer]") {
  WriterFixture fx("e0view_off");

  CodegenWriter writer(*fx.ctx);
  REQUIRE(writer.write(false));

  // Off: the generated header defines no marker (the template's guard only names it) and the image
  // says false, so the code is what it was before the option existed.
  const auto pch = ReadAll(fx.outputDir() / "testproj_pch.h");
  CHECK(pch.find("#define REX_CONFIG_EXACT_E0_VIEW") == std::string::npos);
  CHECK(pch.find("REX_PHYS_HOST_OFFSET(addr) (((u32)(addr) >= 0xE0000000u) ? 0x1000u : 0u)") !=
        std::string::npos);
  const auto init = ReadAll(fx.outputDir() / "testproj_init.cpp");
  CHECK(init.find(".exact_e0_view = false,") != std::string::npos);
}

TEST_CASE("exact_e0_view turns the +0x1000 test off on Windows and tells the runtime",
          "[codegen_writer]") {
  WriterFixture fx("e0view_on");
  fx.ctx->Config().exactE0View = true;

  CodegenWriter writer(*fx.ctx);
  REQUIRE(writer.write(false));

  // The marker comes before every SDK header, so a translation unit that includes the generated
  // header first sees it; the Windows branch of REX_PHYS_HOST_OFFSET is guarded by it.
  const auto pch = ReadAll(fx.outputDir() / "testproj_pch.h");
  const auto marker = pch.find("#define REX_CONFIG_EXACT_E0_VIEW");
  REQUIRE(marker != std::string::npos);
  CHECK(marker < pch.find("#include <rex/ppc.h>"));
  CHECK(pch.find("REX_PLATFORM_WIN32 && !defined(REX_CONFIG_EXACT_E0_VIEW)") != std::string::npos);
  const auto init = ReadAll(fx.outputDir() / "testproj_init.cpp");
  CHECK(init.find(".exact_e0_view = true,") != std::string::npos);
}

TEST_CASE("exact_e0_view is read from the TOML", "[codegen_writer]") {
  const auto dir = fs::temp_directory_path() / "rexglue_writer_test" / "e0view_toml";
  fs::remove_all(dir);
  fs::create_directories(dir);
  {
    std::ofstream out(dir / "on.toml");
    out << "file_path = \"game.xex\"\nexact_e0_view = true\n";
  }
  {
    std::ofstream out(dir / "off.toml");
    out << "file_path = \"game.xex\"\n";
  }

  RecompilerConfig on;
  REQUIRE(on.Load((dir / "on.toml").string()));
  CHECK(on.exactE0View);

  RecompilerConfig off;
  REQUIRE(off.Load((dir / "off.toml").string()));
  CHECK_FALSE(off.exactE0View);

  std::error_code ec;
  fs::remove_all(dir, ec);
}
