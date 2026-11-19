/**
 * @file        tests/unit/gpu/texture_direct_load_test.cpp
 * @brief       Which textures are loaded straight into the host texture, and the pages a resolve
 *              has written at native resolution
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <memory>
#include <utility>
#include <vector>

#include <rex/setting.h>
#include <rex/xenos/fetch.h>
#include <rex/xenos/format.h>
#include <rex/xenos/register.h>

#include "fake_title_context.h"
#include "gpu/xenos/pipeline/texture/cache.h"
#include "gpu/xenos/shared_memory.h"
#include "gpu/xenos/xenos_settings.h"
#include "test_memory.h"

namespace {

using rex::test::FakeTitleContext;
namespace xenos = rex::xenos;

struct Fixture {
  Fixture() { rex::test::ReleaseTestMemory(); }
  ~Fixture() { rex::test::ReleaseTestMemory(); }
};

struct TestXenosSettings final : rex::gpu::XenosSettings {};

/** A shared memory with nothing behind it: an upload marks its pages valid, as the backends do. */
class TestSharedMemory final : public rex::gpu::SharedMemory {
 public:
  TestSharedMemory(FakeTitleContext& title, const rex::gpu::XenosSettings& settings)
      : SharedMemory(title, title, settings) {
    InitializeCommon();
  }

 protected:
  bool UploadRanges(const std::vector<std::pair<rex::u32, rex::u32>>& ranges) override {
    for (const auto& [start, count] : ranges) {
      MarkRangeValid(start << GetPageSizeLog2(), count << GetPageSizeLog2(), false);
    }
    return true;
  }
};

/** A texture cache whose textures are empty shells: only the keys it makes matter. */
class TestTextureCache final : public rex::gpu::TextureCache {
 public:
  using TextureCache::DirectLoadShader;
  using TextureCache::FindOrCreateTexture;
  using TextureCache::GetDirectLoadShader;
  using TextureCache::IsDirectLoadPossible;
  using TextureCache::IsDirectLoadUnscaledWanted;
  using TextureCache::LoadShaderIndex;
  using TextureCache::Texture;
  using TextureCache::TextureKey;

  // The enumerators are protected members of the base: these name them for the tests.
  static constexpr LoadShaderIndex kLoad8 = kLoadShaderIndex8bpb;
  static constexpr LoadShaderIndex kLoad32 = kLoadShaderIndex32bpb;
  static constexpr LoadShaderIndex kLoad64 = kLoadShaderIndex64bpb;
  static constexpr LoadShaderIndex kLoad128 = kLoadShaderIndex128bpb;
  static constexpr LoadShaderIndex kLoadDepthFloat = kLoadShaderIndexDepthFloat;
  static constexpr LoadShaderIndex kLoadDepthUnorm = kLoadShaderIndexDepthUnorm;
  static constexpr LoadShaderIndex kLoadDXT1 = kLoadShaderIndexDXT1ToRGBA8;
  static constexpr DirectLoadShader kDirect32 = kDirectLoadShader32bpb;
  static constexpr DirectLoadShader kDirect64 = kDirectLoadShader64bpb;
  static constexpr DirectLoadShader kDirectDepthFloat = kDirectLoadShaderDepthFloat;
  static constexpr DirectLoadShader kDirectDepthUnorm = kDirectLoadShaderDepthUnorm;
  static constexpr DirectLoadShader kDirectUnknown = kDirectLoadShaderUnknown;

  TestTextureCache(const xenos::RegisterFile& registers, const rex::gpu::XenosSettings& settings,
                   rex::gpu::SharedMemory& shared_memory, rex::u32 resolution_scale)
      : TextureCache(registers, settings, shared_memory, resolution_scale, resolution_scale) {}

 protected:
  class TestTexture final : public Texture {
   public:
    TestTexture(TextureCache& texture_cache, const TextureKey& key) : Texture(texture_cache, key) {}
  };

  rex::u32 GetHostFormatSwizzle(TextureKey) const override { return 0; }
  rex::u32 GetMaxHostTextureWidthHeight(xenos::DataDimension) const override { return 16384; }
  rex::u32 GetMaxHostTextureDepthOrArraySize(xenos::DataDimension) const override { return 2048; }
  std::unique_ptr<Texture> CreateTexture(TextureKey key) override {
    return std::make_unique<TestTexture>(*this, key);
  }
  bool LoadHostTextureDataFromResidentMemory(Texture&, bool, bool) override { return true; }
};

constexpr rex::u32 kBasePage = 0x100;

/** A tiled 256x256 single-level 2D texture of 32 bits a texel: 64 pages from kBasePage. */
TestTextureCache::TextureKey MakeKey() {
  TestTextureCache::TextureKey key;
  key.base_page = kBasePage;
  key.dimension = xenos::DataDimension::k2DOrStacked;
  key.width_minus_1 = 255;
  key.height_minus_1 = 255;
  key.tiled = 1;
  key.packed_mips = 1;
  key.pitch = 256 / 32;
  key.format = xenos::TextureFormat::k_8_8_8_8;
  key.endianness = xenos::Endian::k8in32;
  key.is_valid = 1;
  return key;
}

constexpr rex::u32 kKeyPages = 256 * 256 * 4 / 4096;

struct Cache {
  FakeTitleContext title;
  TestXenosSettings settings;
  TestSharedMemory shared;
  xenos::RegisterFile registers;
  TestTextureCache cache;

  explicit Cache(rex::u32 resolution_scale = 1)
      : title(rex::test::GetTestMemory()),
        shared(title, settings),
        cache(registers, settings, shared, resolution_scale) {}

  bool Resolved(TestTextureCache::TextureKey key) {
    return cache.FindOrCreateTexture(key)->GetKey().resolved_unscaled != 0;
  }
};

void Resolve(Cache& c, rex::u32 first_page, rex::u32 page_count) {
  c.cache.MarkRangeAsResolved(first_page << 12, page_count << 12);
}

}  // namespace

REX_TEST_CASE("texture direct load: the key has room for the resolved flag", "[gpu][texture]") {
  TestTextureCache::TextureKey key = MakeKey();
  TestTextureCache::TextureKey resolved = key;
  CHECK(key == resolved);
  resolved.resolved_unscaled = 1;
  CHECK(key != resolved);
  CHECK(key.resolved_unscaled == 0);
  CHECK(sizeof(TestTextureCache::TextureKey) == 16);
}

REX_TEST_CASE("texture direct load: the load shaders that have a direct one", "[gpu][texture]") {
  using T = TestTextureCache;
  CHECK(T::GetDirectLoadShader(T::kLoad32) == T::kDirect32);
  CHECK(T::GetDirectLoadShader(T::kLoad64) == T::kDirect64);
  CHECK(T::GetDirectLoadShader(T::kLoadDepthFloat) == T::kDirectDepthFloat);
  CHECK(T::GetDirectLoadShader(T::kLoadDepthUnorm) == T::kDirectDepthUnorm);
  // The conversions and the other sizes have none.
  CHECK(T::GetDirectLoadShader(T::kLoad8) == T::kDirectUnknown);
  CHECK(T::GetDirectLoadShader(T::kLoad128) == T::kDirectUnknown);
  CHECK(T::GetDirectLoadShader(T::kLoadDXT1) == T::kDirectUnknown);
}

REX_TEST_CASE_METHOD(Fixture, "texture direct load: a scaled texture is loaded straight in",
                     "[gpu][texture]") {
  Cache c(2);
  TestTextureCache::TextureKey key = MakeKey();
  // Not scaled and not resolved: through the buffer.
  CHECK_FALSE(c.cache.IsDirectLoadPossible(key));
  key.scaled_resolve = 1;
  CHECK(c.cache.IsDirectLoadPossible(key));

  // One level, 2D, one layer, not in the packed tail.
  TestTextureCache::TextureKey mips = key;
  mips.mip_max_level = 3;
  CHECK_FALSE(c.cache.IsDirectLoadPossible(mips));
  TestTextureCache::TextureKey layers = key;
  layers.depth_or_array_size_minus_1 = 1;
  CHECK_FALSE(c.cache.IsDirectLoadPossible(layers));
  TestTextureCache::TextureKey volume = key;
  volume.dimension = xenos::DataDimension::k3D;
  CHECK_FALSE(c.cache.IsDirectLoadPossible(volume));
  TestTextureCache::TextureKey tail = key;
  tail.width_minus_1 = 15;
  tail.height_minus_1 = 15;
  CHECK_FALSE(c.cache.IsDirectLoadPossible(tail));

  // Not the native resolution cache's business.
  CHECK_FALSE(c.cache.IsDirectLoadUnscaledWanted());
}

REX_TEST_CASE_METHOD(Fixture, "texture direct load: a native resolution texture needs the flag",
                     "[gpu][texture]") {
  Cache c;
  CHECK(c.cache.IsDirectLoadUnscaledWanted());
  TestTextureCache::TextureKey key = MakeKey();
  CHECK_FALSE(c.cache.IsDirectLoadPossible(key));

  key.resolved_unscaled = 1;
  CHECK(c.cache.IsDirectLoadPossible(key));

  // Only tiled 2D, the scaled shaders assume tiling.
  TestTextureCache::TextureKey linear = key;
  linear.tiled = 0;
  CHECK_FALSE(c.cache.IsDirectLoadPossible(linear));
  TestTextureCache::TextureKey line = key;
  line.dimension = xenos::DataDimension::k1D;
  CHECK_FALSE(c.cache.IsDirectLoadPossible(line));

  REQUIRE(c.settings.texture_load_direct_unscaled.SetText(rex::SettingLayer::kRuntime, "false"));
  CHECK_FALSE(c.cache.IsDirectLoadPossible(key));
  CHECK_FALSE(c.cache.IsDirectLoadUnscaledWanted());
}

REX_TEST_CASE_METHOD(Fixture, "texture direct load: a texture over a resolved page is flagged",
                     "[gpu][texture]") {
  Cache c;
  const TestTextureCache::TextureKey key = MakeKey();
  auto* before = c.cache.FindOrCreateTexture(key);
  REQUIRE(before != nullptr);
  CHECK(before->GetKey().resolved_unscaled == 0);

  // A resolve past the end of the texture's pages is not its own.
  Resolve(c, kBasePage + kKeyPages, 1);
  CHECK_FALSE(c.Resolved(key));
  Resolve(c, kBasePage - 1, 1);
  CHECK_FALSE(c.Resolved(key));

  // One resolved page anywhere in it is enough, and it is a texture of its own: the one made before
  // has no unordered access.
  Resolve(c, kBasePage + kKeyPages - 1, 1);
  auto* after = c.cache.FindOrCreateTexture(key);
  REQUIRE(after != nullptr);
  CHECK(after != before);
  CHECK(after->GetKey().resolved_unscaled == 1);
  CHECK(c.cache.FindOrCreateTexture(key) == after);
}

REX_TEST_CASE_METHOD(Fixture,
                     "texture direct load: only the textures that can be loaded straight in",
                     "[gpu][texture]") {
  Cache c;
  Resolve(c, kBasePage, kKeyPages);
  CHECK(c.Resolved(MakeKey()));

  TestTextureCache::TextureKey linear = MakeKey();
  linear.tiled = 0;
  CHECK_FALSE(c.Resolved(linear));

  TestTextureCache::TextureKey wide = MakeKey();
  wide.format = xenos::TextureFormat::k_16_16_16_16;
  CHECK(c.Resolved(wide));

  // Texels of other sizes and blocks of more than a texel are decoded or converted.
  TestTextureCache::TextureKey small_texels = MakeKey();
  small_texels.format = xenos::TextureFormat::k_5_6_5;
  CHECK_FALSE(c.Resolved(small_texels));
  TestTextureCache::TextureKey compressed = MakeKey();
  compressed.format = xenos::TextureFormat::k_DXT1;
  CHECK_FALSE(c.Resolved(compressed));

  TestTextureCache::TextureKey mips = MakeKey();
  mips.mip_max_level = 3;
  mips.mip_page = kBasePage + kKeyPages;
  CHECK_FALSE(c.Resolved(mips));
  TestTextureCache::TextureKey layers = MakeKey();
  layers.depth_or_array_size_minus_1 = 1;
  CHECK_FALSE(c.Resolved(layers));
  TestTextureCache::TextureKey volume = MakeKey();
  volume.dimension = xenos::DataDimension::k3D;
  CHECK_FALSE(c.Resolved(volume));

  // A texture of 16 texels or less is in the packed tail.
  TestTextureCache::TextureKey tiny = MakeKey();
  tiny.width_minus_1 = 15;
  tiny.height_minus_1 = 15;
  tiny.pitch = 1;
  CHECK_FALSE(c.Resolved(tiny));
}

REX_TEST_CASE_METHOD(Fixture, "texture direct load: nothing is flagged when it is off",
                     "[gpu][texture]") {
  FakeTitleContext title(rex::test::GetTestMemory());
  TestXenosSettings settings;
  REQUIRE(settings.texture_load_direct_unscaled.SetText(rex::SettingLayer::kRuntime, "false"));
  TestSharedMemory shared(title, settings);
  xenos::RegisterFile registers;
  TestTextureCache cache(registers, settings, shared, 1);

  cache.MarkRangeAsResolved(kBasePage << 12, kKeyPages << 12);
  CHECK(cache.FindOrCreateTexture(MakeKey())->GetKey().resolved_unscaled == 0);
}

REX_TEST_CASE_METHOD(Fixture, "texture direct load: a scaled cache keeps its own resolved pages",
                     "[gpu][texture]") {
  Cache c(2);
  Resolve(c, kBasePage, kKeyPages);
  // The scaled resolve bits are what a scaled cache goes by, and the textures of this one cannot be
  // scaled resolve (the test host supports none), so the native flag stays clear.
  CHECK_FALSE(c.Resolved(MakeKey()));
}
