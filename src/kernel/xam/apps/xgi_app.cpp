/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/apps/xgi_app.h>
#include <rex/logging.h>
#include <rex/thread.h>
#include "../mcla_online.h"
#include "../mcla_online_protocol.h"

#include <algorithm>
#include <cstring>

#if REX_PLATFORM_WIN32
#include <windows.h>
#else
#include <rex/net/posix_compat.h>  // GetTickCount
#endif

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XgiApp::XgiApp(KernelState* kernel_state) : App(kernel_state, 0xFB) {}

namespace {

// XUSER_CONTEXT is {DWORD id, DWORD value} = 8 bytes.
constexpr uint32_t kContextSize = 8;
// XUSER_PROPERTY (Xbox 360, natural alignment):
//   +0x00 DWORD    dwPropertyId
//   +0x04 (pad)
//   +0x08 BYTE     value.type        (XUSER_DATA_TYPE; = id high nibble)
//   +0x10 union    value             (nData/i64Data/... at +16 because the
//                                      union holds an 8-byte-aligned LONGLONG)
// sizeof = 24. Emitting 16-byte-strided entries made the game's reader walk off
// the end into adjacent heap (garbage remote content mask), so the stride must
// be the full 24.
constexpr uint32_t kPropertySize = 24;
constexpr uint32_t kPropTypeOffset = 8;
constexpr uint32_t kPropValueOffset = 16;

// Guest pointers + counts for one search result's context/property arrays.
struct ResultData {
  uint32_t ctx_guest = 0;
  uint16_t ctx_count = 0;
  uint32_t prop_guest = 0;
  uint16_t prop_count = 0;
};

// Builds the context and property arrays a joiner reads from a search result to
// rebuild the event definition (content mask, vehicle class, max players, ...).
// Session ids are split by their high nibble: nibble 0 = XUSER_CONTEXT (game
// mode/type), non-zero = XUSER_PROPERTY (Content0..7, VehicleClass, MaxPlayers,
// ... are all property-range ids). Emitting the property-range ids as contexts
// does NOT populate the game's property blob, which is why they must go through
// pProperties. If the host carried no props (old client / empty store) we fall
// back to echoing the caller's search contexts so the session still matches.
ResultData EmitResultData(rex::memory::Memory* memory, const mclaonline::SessionInfo& s,
                          uint32_t ctx_ptr, uint16_t num_ctx) {
  ResultData out;

  uint16_t n = s.prop_count;
  if (n > mclaonline::kMaxSessionProps) {
    n = mclaonline::kMaxSessionProps;
  }

  if (n == 0) {
    // Fallback: echo the caller's own search contexts.
    if (num_ctx > 0 && ctx_ptr) {
      out.ctx_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(num_ctx * kContextSize);
      auto* block = memory->TranslateVirtual(out.ctx_guest);
      auto* src = memory->TranslateVirtual(ctx_ptr);
      std::memcpy(block, src, static_cast<size_t>(num_ctx) * kContextSize);
      out.ctx_count = num_ctx;
    }
    return out;
  }

  // Count how many go to each array.
  uint16_t n_ctx = 0;
  uint16_t n_prop = 0;
  for (uint16_t j = 0; j < n; ++j) {
    if ((s.props[j].id >> 28) == 0) {
      ++n_ctx;
    } else {
      ++n_prop;
    }
  }

  if (n_ctx > 0) {
    out.ctx_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(n_ctx * kContextSize);
    std::memset(memory->TranslateVirtual(out.ctx_guest), 0, n_ctx * kContextSize);
  }
  if (n_prop > 0) {
    out.prop_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(n_prop * kPropertySize);
    // Zero first: if the layout is still slightly off, the reader sees clean
    // zeros instead of adjacent-heap garbage.
    std::memset(memory->TranslateVirtual(out.prop_guest), 0, n_prop * kPropertySize);
  }

  uint16_t ci = 0;
  uint16_t pi = 0;
  for (uint16_t j = 0; j < n; ++j) {
    uint32_t id = s.props[j].id;
    uint32_t value = s.props[j].value;
    if ((id >> 28) == 0) {
      auto* e = memory->TranslateVirtual(out.ctx_guest + ci * kContextSize);
      memory::store_and_swap<uint32_t>(e + 0, id);
      memory::store_and_swap<uint32_t>(e + 4, value);
      ++ci;
    } else {
      auto* e = memory->TranslateVirtual(out.prop_guest + pi * kPropertySize);
      memory::store_and_swap<uint32_t>(e + 0, id);                     // dwPropertyId
      e[kPropTypeOffset] = static_cast<uint8_t>(id >> 28);             // value.type
      memory::store_and_swap<uint32_t>(e + kPropValueOffset, value);   // value.nData (INT32)
      ++pi;
    }
  }
  out.ctx_count = n_ctx;
  out.prop_count = n_prop;
  return out;
}

}  // namespace

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x000B0006: {
      assert_true(!buffer_length || buffer_length == 24);
      // dword r3 user index
      // dword (unwritten?)
      // qword 0
      // dword r4 context enum
      // dword r5 value
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t context_value = memory::load_and_swap<uint32_t>(buffer + 20);
      REXKRNL_INFO("XGIUserSetContextEx(user={:08X}, ctx={:08X}, val={:08X})", user_index, context_id,
                    context_value);
      // Capture the real value at set time so the advertised session carries the
      // event definition (search-result properties). A one-shot store read at
      // XNetQosListen runs before the game sets these, so it captured defaults.
      if (OnlineClient::Enabled()) {
        OnlineClient::Get().UpdateSessionProp(context_id, context_value);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0007: {
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t property_id = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t value_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t value_ptr = memory::load_and_swap<uint32_t>(buffer + 24);
      REXKRNL_INFO("XGIUserSetPropertyEx(user={:08X}, prop={:08X}, size={}, ptr={:08X})", user_index, property_id,
                    value_size, value_ptr);
      // Capture 32-bit property values as the game sets them (see SetContextEx).
      // 64-bit properties (e.g. HostUID) don't gate the event; skip them.
      if (OnlineClient::Enabled() && value_size == 4 && value_ptr) {
        uint32_t v = memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(value_ptr));
        OnlineClient::Get().UpdateSessionProp(property_id, v);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0008: {
      // Raw dump so we can confirm the actual buffer layout the game sends.
      uint32_t raw0 = buffer_length >= 4 ? memory::load_and_swap<uint32_t>(buffer + 0) : 0;
      uint32_t raw4 = buffer_length >= 8 ? memory::load_and_swap<uint32_t>(buffer + 4) : 0;
      REXKRNL_INFO("XGIUserWriteAchievements called: buf_len={} raw[0]={:08X} raw[4]={:08X}",
                   buffer_length, raw0, raw4);

      assert_true(!buffer_length || buffer_length == 8);
      uint32_t achievement_count = raw0;
      uint32_t achievements_ptr = raw4;

      // Empirically confirmed from log: each entry is {u32 padding/user_index, u32 id, ...}.
      // The achievement ID sits at offset 4, not 0. Stride 8 covers the observed fields.
      constexpr uint32_t kEntryIdOffset = 4;
      constexpr uint32_t kEntryStride = 8;
      constexpr uint32_t kMaxAchievements = 1000;

      if (achievements_ptr && achievement_count > 0) {
        if (achievement_count > kMaxAchievements) {
          REXKRNL_WARN("XGIUserWriteAchievements: count={} unreasonable, ignoring",
                       achievement_count);
          return X_E_FAIL;
        }
        uint32_t span_end = achievements_ptr + achievement_count * kEntryStride - 1;
        if (!memory_->LookupHeap(achievements_ptr) || !memory_->LookupHeap(span_end)) {
          REXKRNL_WARN("XGIUserWriteAchievements: ptr {:08X} OOB", achievements_ptr);
          return X_E_FAIL;
        }
        auto* base = memory_->TranslateVirtual(achievements_ptr);
        for (uint32_t i = 0; i < achievement_count; ++i) {
          uint32_t id = memory::load_and_swap<uint32_t>(base + i * kEntryStride + kEntryIdOffset);
          REXKRNL_INFO("XGIUserWriteAchievements: id={} ({})", id, i);
          kernel_state_->UnlockAchievement(id);
        }
      } else {
        REXKRNL_INFO("XGIUserWriteAchievements: skipped (count={} ptr={:08X})", achievement_count,
                     achievements_ptr);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0010: {
      assert_true(!buffer_length || buffer_length == 28);
      // Sequence:
      // - XamSessionCreateHandle
      // - XamSessionRefObjByHandle
      // - [this]
      // - CloseHandle
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_slots_public = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t num_slots_private = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t user_xuid = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t nonce_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG(
          "XGISessionCreateImpl({:08X}, {:08X}, {}, {}, {:08X}, {:08X}, "
          "{:08X})",
          session_ptr, flags, num_slots_public, num_slots_private, user_xuid, session_info_ptr,
          nonce_ptr);

      if (session_ptr) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(session_ptr), 0xDEADF00D);
      }

      if (session_info_ptr) {
        auto session_info = memory_->TranslateVirtual(session_info_ptr);

        auto& oc = OnlineClient::Get();
        oc.EnsureStarted();
        uint64_t sid = oc.session_id();
        uint32_t my_ip = 0x7F000001; // localhost fallback
        uint32_t vip = 0;
        
        if (oc.Enabled()) {
          if (!oc.connection_checked()) {
            uint32_t elapsed = GetTickCount() - oc.startup_time_ms();
            if (elapsed < 250) {
              uint32_t remaining = 250 - elapsed;
              for (uint32_t i = 0; i < remaining / 5 && !oc.connection_checked(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
              }
            }
            if (!oc.connection_checked()) {
              oc.set_connection_checked(true);
            }
          }
          if (oc.server_online()) {
            vip = oc.virtual_ip();
          }
        }

        if (vip != 0) {
          my_ip = vip;
        }

        // Write sessionID (uint64_t)
        memory::store_and_swap<uint64_t>(session_info + 0, sid);

        // Write hostAddress.ina (uint32_t)
        memory::store_and_swap<uint32_t>(session_info + 8, my_ip);
        // Write hostAddress.inaOnline (uint32_t)
        memory::store_and_swap<uint32_t>(session_info + 12, my_ip);
        // Write hostAddress.wPortOnline (uint16_t)
        uint16_t game_port = oc.game_port();
        if (game_port == 0) game_port = 1000;
        memory::store_and_swap<uint16_t>(session_info + 16, game_port);

        // Write hostAddress.abEnet (6 bytes)
        uint8_t mac[6] = {0xCC, 0xCC, 0xCC, 0xCC, static_cast<uint8_t>((my_ip >> 8) & 0xFF), static_cast<uint8_t>(my_ip & 0xFF)};
        std::memcpy(session_info + 18, mac, 6);

        // Write hostAddress.abOnline (20 bytes)
        std::memset(session_info + 24, 0, 20);

        // Write keyExchangeKey (16 bytes)
        uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        std::memcpy(session_info + 44, key, 16);
      }

      if (nonce_ptr) {
        auto nonce = memory_->TranslateVirtual(nonce_ptr);
        uint64_t nonce_val = 0x1122334455667788ull;
        memory::store_and_swap<uint64_t>(nonce, nonce_val);
      }

      return X_E_SUCCESS;
    }
    case 0x000B0011: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_DEBUG("XGISessionDelete({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return X_E_SUCCESS;
    }
    case 0x000B0012: {
      assert_true(!buffer_length || buffer_length == 20);
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuid_array_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t user_index_array = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t private_slots_array = memory::load_and_swap<uint32_t>(buffer + 16);
      bool is_local = xuid_array_ptr == 0;

      REXKRNL_DEBUG("{}({:08X}, {}, {}, {:08X}, {:08X})",
                    is_local ? "XGISessionJoinLocal" : "XGISessionJoinRemote", session_ptr,
                    user_count, xuid_array_ptr, user_index_array, private_slots_array);
      return X_E_SUCCESS;
    }
    case 0x000B0013: {
      // XSessionJoinRemote(session, xuid_count, pXuids, pfPrivateSlots). The
      // actual join step: without it Quick Cruise search finds the host but the
      // join call fails, so the game falls back to creating its own session.
      // We track players over the game's own relayed networking, so accepting
      // the join is enough here; the arbitration/player list is not emulated.
      assert_true(!buffer_length || buffer_length == 20);
      uint32_t session_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t xuid_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t unk_0 = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t xuid_array = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t private_slots_array = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_INFO("XGISessionJoinRemote({:08X}, {}, {}, {:08X}, {:08X})", session_ptr, xuid_count,
                   unk_0, xuid_array, private_slots_array);
      return X_E_SUCCESS;
    }
    case 0x000B0014: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_DEBUG("XSessionStart({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return X_STATUS_SUCCESS;
    }
    case 0x000B0015: {
      // send high scores?
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);

      REXKRNL_DEBUG("XSessionEnd({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return X_E_SUCCESS;
    }
    case 0x000B0016: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t props_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t ctx_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_INFO("XSessionSearch({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X})", proc_index,
                    user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                    results_buffer_size, search_results_ptr);

      if (ctx_ptr && num_ctx > 0) {
        auto* ctx = memory_->TranslateVirtual(ctx_ptr);
        for (uint16_t ci = 0; ci < num_ctx; ++ci) {
          uint32_t ctx_id = memory::load_and_swap<uint32_t>(ctx + ci * 8);
          uint32_t ctx_val = memory::load_and_swap<uint32_t>(ctx + ci * 8 + 4);
          REXKRNL_INFO("  search ctx[{}]: id={:08X} val={:08X}", ci, ctx_id, ctx_val);
        }
      }
      if (props_ptr && num_props > 0) {
        auto* props = memory_->TranslateVirtual(props_ptr);
        for (uint16_t pi = 0; pi < num_props; ++pi) {
          uint32_t prop_id = memory::load_and_swap<uint32_t>(props + pi * 16);
          REXKRNL_INFO("  search prop[{}]: id={:08X}", pi, prop_id);
        }
      }

      if (search_results_ptr && OnlineClient::Enabled()) {
        auto sessions = OnlineClient::Get().QuerySessions(500);
        auto header = memory_->TranslateVirtual(search_results_ptr);

        // XSESSION_SEARCHRESULT layout (per entry):
        //   XSESSION_INFO info (60 bytes: sessionID[8] + XNADDR[36] + XNKEY[16])
        //   DWORD dwOpenPublicSlots, dwOpenPrivateSlots,
        //         dwFilledPublicSlots, dwFilledPrivateSlots   (16 bytes)
        //   DWORD cProperties, cContexts                      (8 bytes)
        //   ptr   pProperties, pContexts                      (8 bytes)
        // Total per result: 92 bytes
        constexpr uint32_t kResultSize = 92;
        constexpr uint32_t kHeaderSize = 8;  // dwSearchResults + pResults

        uint32_t max_fit = 0;
        if (results_buffer_size > kHeaderSize) {
          max_fit = (results_buffer_size - kHeaderSize) / kResultSize;
        }
        uint32_t count = std::min({static_cast<uint32_t>(sessions.size()),
                                   num_results, max_fit});

        uint32_t results_guest = search_results_ptr + kHeaderSize;
        memory::store_and_swap<uint32_t>(header + 0, count);
        memory::store_and_swap<uint32_t>(header + 4, count > 0 ? results_guest : 0);

        for (uint32_t i = 0; i < count; ++i) {
          auto& s = sessions[i];
          auto* r = memory_->TranslateVirtual(results_guest + i * kResultSize);

          // XSESSION_INFO.sessionID (8 bytes)
          memory::store_and_swap<uint64_t>(r + 0, s.session_id);
          // XSESSION_INFO.hostAddress.ina (4 bytes, network order)
          memory::store_and_swap<uint32_t>(r + 8, s.virtual_ip);
          // XSESSION_INFO.hostAddress.inaOnline (4 bytes)
          memory::store_and_swap<uint32_t>(r + 12, s.virtual_ip);
          // XSESSION_INFO.hostAddress.wPortOnline (2 bytes)
          memory::store_and_swap<uint16_t>(r + 16, s.host_port);
          // XSESSION_INFO.hostAddress.abEnet (6 bytes)
          uint8_t mac[6] = {0xCC, 0xCC, 0xCC, 0xCC,
                            static_cast<uint8_t>((s.virtual_ip >> 8) & 0xFF),
                            static_cast<uint8_t>(s.virtual_ip & 0xFF)};
          std::memcpy(r + 18, mac, 6);
          // XSESSION_INFO.hostAddress.abOnline (20 bytes)
          std::memset(r + 24, 0, 20);
          // XSESSION_INFO.keyExchangeKey (16 bytes)
          uint8_t key[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
          std::memcpy(r + 44, key, 16);

          // dwOpenPublicSlots
          memory::store_and_swap<uint32_t>(r + 60, s.max_players - s.cur_players);
          // dwOpenPrivateSlots
          memory::store_and_swap<uint32_t>(r + 64, 0);
          // dwFilledPublicSlots
          memory::store_and_swap<uint32_t>(r + 68, s.cur_players);
          // dwFilledPrivateSlots
          memory::store_and_swap<uint32_t>(r + 72, 0);
          // Host's real session properties (event definition), split into
          // contexts + properties by id nibble; fallback echoes the caller's
          // search contexts. See EmitResultData.
          ResultData rd = EmitResultData(memory_, s, ctx_ptr, num_ctx);
          memory::store_and_swap<uint32_t>(r + 76, rd.prop_count);  // cProperties
          memory::store_and_swap<uint32_t>(r + 80, rd.ctx_count);   // cContexts
          memory::store_and_swap<uint32_t>(r + 84, rd.prop_guest);  // pProperties
          memory::store_and_swap<uint32_t>(r + 88, rd.ctx_guest);   // pContexts
        }

        REXKRNL_INFO(
            "XSessionSearch: returning {} sessions from relay ({} host props on first)",
            count, count > 0 ? sessions[0].prop_count : 0);
      } else if (search_results_ptr) {
        auto header = memory_->TranslateVirtual(search_results_ptr);
        memory::store_and_swap<uint32_t>(header + 0, 0);
        memory::store_and_swap<uint32_t>(header + 4, 0);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0018: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t maxPublicSlots = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t maxPrivateSlots = memory::load_and_swap<uint16_t>(buffer + 12);

      REXKRNL_DEBUG("XSessionModify({:08X}, {:08X}, {:08X}, {:08X})", obj_ptr, flags,
                    maxPublicSlots, maxPrivateSlots);

      return X_E_SUCCESS;
    }
    case 0x000B001C: {
      assert_true(!buffer_length || buffer_length == 36);

      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t props_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t ctx_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 28);
      uint32_t num_users = memory::load_and_swap<uint32_t>(buffer + 32);

      REXKRNL_INFO("XSessionSearchEx({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X}, {})",
                    proc_index, user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                    results_buffer_size, search_results_ptr, num_users);

      if (ctx_ptr && num_ctx > 0) {
        auto* ctx = memory_->TranslateVirtual(ctx_ptr);
        for (uint16_t ci = 0; ci < num_ctx; ++ci) {
          uint32_t ctx_id = memory::load_and_swap<uint32_t>(ctx + static_cast<size_t>(ci) * 8);
          uint32_t ctx_val = memory::load_and_swap<uint32_t>(ctx + static_cast<size_t>(ci) * 8 + 4);
          REXKRNL_INFO("  searchEx ctx[{}]: id={:08X} val={:08X}", ci, ctx_id, ctx_val);
        }
      }

      if (search_results_ptr && OnlineClient::Enabled()) {
        auto sessions = OnlineClient::Get().QuerySessions(500);
        auto header = memory_->TranslateVirtual(search_results_ptr);

        constexpr uint32_t kResultSize = 92;
        constexpr uint32_t kHeaderSize = 8;

        uint32_t max_fit = 0;
        if (results_buffer_size > kHeaderSize) {
          max_fit = (results_buffer_size - kHeaderSize) / kResultSize;
        }
        uint32_t count = std::min({static_cast<uint32_t>(sessions.size()),
                                   num_results, max_fit});

        uint32_t results_guest = search_results_ptr + kHeaderSize;
        memory::store_and_swap<uint32_t>(header + 0, count);
        memory::store_and_swap<uint32_t>(header + 4, count > 0 ? results_guest : 0);

        for (uint32_t i = 0; i < count; ++i) {
          auto& s = sessions[i];
          auto* r = memory_->TranslateVirtual(results_guest + i * kResultSize);

          memory::store_and_swap<uint64_t>(r + 0, s.session_id);
          memory::store_and_swap<uint32_t>(r + 8, s.virtual_ip);
          memory::store_and_swap<uint32_t>(r + 12, s.virtual_ip);
          memory::store_and_swap<uint16_t>(r + 16, s.host_port);
          uint8_t mac[6] = {0xCC, 0xCC, 0xCC, 0xCC,
                            static_cast<uint8_t>((s.virtual_ip >> 8) & 0xFF),
                            static_cast<uint8_t>(s.virtual_ip & 0xFF)};
          std::memcpy(r + 18, mac, 6);
          std::memset(r + 24, 0, 20);
          uint8_t key[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
          std::memcpy(r + 44, key, 16);
          memory::store_and_swap<uint32_t>(r + 60, s.max_players - s.cur_players);
          memory::store_and_swap<uint32_t>(r + 64, 0);
          memory::store_and_swap<uint32_t>(r + 68, s.cur_players);
          memory::store_and_swap<uint32_t>(r + 72, 0);
          // Emit the host's real session properties (event definition) so the
          // joiner rebuilds content mask / vehicle class / etc. Split into
          // contexts + properties by id nibble; see EmitResultData.
          ResultData rd = EmitResultData(memory_, s, ctx_ptr, num_ctx);
          memory::store_and_swap<uint32_t>(r + 76, rd.prop_count);  // cProperties
          memory::store_and_swap<uint32_t>(r + 80, rd.ctx_count);   // cContexts
          memory::store_and_swap<uint32_t>(r + 84, rd.prop_guest);  // pProperties
          memory::store_and_swap<uint32_t>(r + 88, rd.ctx_guest);   // pContexts
        }

        REXKRNL_INFO(
            "XSessionSearchEx: returning {} sessions from relay ({} host props on first)",
            count, count > 0 ? sessions[0].prop_count : 0);
      } else if (search_results_ptr) {
        auto header = memory_->TranslateVirtual(search_results_ptr);
        memory::store_and_swap<uint32_t>(header + 0, 0);
        memory::store_and_swap<uint32_t>(header + 4, 0);
      }
      return X_E_SUCCESS;
    }
    case 0x000B001D: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t details_buffer_size = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_details_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionGetDetails({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr,
                    details_buffer_size, session_details_ptr, reserved1, reserved2, reserved3);

      if (session_details_ptr) {
        auto details = memory_->TranslateVirtual(session_details_ptr);

        auto& oc = OnlineClient::Get();
        oc.EnsureStarted();
        uint64_t sid = oc.session_id();
        uint32_t my_ip = 0x7F000001; // localhost fallback
        uint32_t vip = oc.virtual_ip();
        for (int i = 0; i < 200 && vip == 0; ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          vip = oc.virtual_ip();
        }
        if (vip != 0) {
          my_ip = vip;
        }

        // dwUserIndexHost (DWORD)
        memory::store_and_swap<uint32_t>(details + 0, 0);
        // dwGameType (DWORD)
        memory::store_and_swap<uint32_t>(details + 4, 0);
        // dwGameMode (DWORD)
        memory::store_and_swap<uint32_t>(details + 8, 0);
        // dwFlags (DWORD)
        memory::store_and_swap<uint32_t>(details + 12, 0);
        // dwMaxPublicSlots (DWORD)
        memory::store_and_swap<uint32_t>(details + 16, 8);
        // dwMaxPrivateSlots (DWORD)
        memory::store_and_swap<uint32_t>(details + 20, 0);
        // dwAvailablePublicSlots (DWORD)
        memory::store_and_swap<uint32_t>(details + 24, 8);
        // dwAvailablePrivateSlots (DWORD)
        memory::store_and_swap<uint32_t>(details + 28, 0);
        // dwActualMemberCount (DWORD)
        memory::store_and_swap<uint32_t>(details + 32, 1);
        // dwReturnedMemberCount (DWORD)
        memory::store_and_swap<uint32_t>(details + 36, 1);
        // eState (XSESSION_STATE, lobby = 1)
        memory::store_and_swap<uint32_t>(details + 40, 1);
        // qwNonce (ULONGLONG)
        memory::store_and_swap<uint64_t>(details + 44, 0x1122334455667788ull);

        // sessionInfo (XSESSION_INFO) - Offset 52
        // Write sessionID (uint64_t)
        memory::store_and_swap<uint64_t>(details + 52, sid);

        // Write hostAddress.ina (uint32_t)
        memory::store_and_swap<uint32_t>(details + 60, my_ip);
        // Write hostAddress.inaOnline (uint32_t)
        memory::store_and_swap<uint32_t>(details + 64, my_ip);
        // Write hostAddress.wPortOnline (uint16_t)
        uint16_t game_port = oc.game_port();
        if (game_port == 0) game_port = 1000;
        memory::store_and_swap<uint16_t>(details + 68, game_port);

        // Write hostAddress.abEnet (6 bytes)
        uint8_t mac[6] = {0xCC, 0xCC, 0xCC, 0xCC, static_cast<uint8_t>((my_ip >> 8) & 0xFF), static_cast<uint8_t>(my_ip & 0xFF)};
        std::memcpy(details + 70, mac, 6);

        // Write hostAddress.abOnline (20 bytes)
        std::memset(details + 76, 0, 20);

        // Write keyExchangeKey (16 bytes)
        uint8_t key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        std::memcpy(details + 96, key, 16);

        // xnkidArbitration (XNKID) - Offset 112 (8 bytes)
        std::memset(details + 112, 0, 8);

        // pSessionMembers (XSESSION_MEMBER*) - Offset 120 (4 bytes in 32-bit guest)
        memory::store_and_swap<uint32_t>(details + 120, 0);
      }

      return X_E_SUCCESS;
    }
    case 0x000B001E: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionMigrateHost({:08X}, {:08X}, {}, {}, {}, {})", obj_ptr,
                    session_info_ptr, user_index, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0019: {
      assert_true(!buffer_length || buffer_length == 8);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t session_info_ptr = memory::load_and_swap<uint32_t>(buffer + 4);

      REXKRNL_DEBUG("XSessionGetInvitationData - unimplemented({}, {:08X})", user_index,
                    session_info_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B001A: {
      assert_true(!buffer_length || buffer_length == 28);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t flags = memory::load_and_swap<uint32_t>(buffer + 4);
      uint64_t session_nonce = memory::load_and_swap<uint64_t>(buffer + 8);
      uint32_t session_duration_sec = memory::load_and_swap<uint32_t>(buffer + 16);  // 300
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG("XSessionArbitrationRegister({:08X}, {:08X}, {:016X}, {:08X}, {:08X}, {:08X})",
                    obj_ptr, flags, session_nonce, session_duration_sec, results_buffer_size,
                    results_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B001B: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t num_session_ids = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_ids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearchByID({}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                    user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                    search_results_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B001F: {
      assert_true(!buffer_length || buffer_length == 24);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t array_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuid_array_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 20);

      REXKRNL_DEBUG("XSessionModifySkill({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr, array_count,
                    xuid_array_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0020: {
      assert_true(!buffer_length || buffer_length == 8);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t view_id = memory::load_and_swap<uint32_t>(buffer + 4);

      REXKRNL_DEBUG("XUserResetStatsView({:08X}, {})", user_index, view_id);

      return X_E_SUCCESS;
    }
    case 0x000B0021: {
      assert_true(!buffer_length || buffer_length == 28);

      uint32_t title_id = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t xuids_count = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t xuids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t specs_count = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t specs_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t results_size = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_ptr = memory::load_and_swap<uint32_t>(buffer + 24);

      REXKRNL_DEBUG("XUserReadStats({}, {}, {:08X}, {}, {:08X}, {}, {:08X})", title_id, xuids_count,
                    xuids_ptr, specs_count, specs_ptr, results_size, results_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0025: {
      assert_true(!buffer_length || buffer_length == 20);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint64_t xuid = memory::load_and_swap<uint64_t>(buffer + 4);
      uint32_t num_views = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t views_ptr = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_DEBUG("XSessionWriteStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                    views_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0026: {
      assert_true(!buffer_length || buffer_length == 20);

      uint32_t obj_ptr = memory::load_and_swap<uint32_t>(buffer + 0);
      uint64_t xuid = memory::load_and_swap<uint64_t>(buffer + 4);
      uint32_t num_views = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t views_ptr = memory::load_and_swap<uint32_t>(buffer + 16);

      REXKRNL_DEBUG("XSessionFlushStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                    views_ptr);

      return X_E_SUCCESS;
    }
    case 0x000B0036: {
      // Called after opening xbox live arcade and clicking on xbox live v5759
      // to 5787 and called after clicking xbox live in the game library from
      // v6683 to v6717
      // Does not get sent a buffer
      REXKRNL_DEBUG("XInvalidateGamerTileCache, unimplemented");
      return X_E_FAIL;
    }
    case 0x000B003D: {
      assert_true(!buffer_length || buffer_length == 16);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t AnId_buffer_size = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t AnId_buffer_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t block = memory::load_and_swap<uint32_t>(buffer + 12);

      REXKRNL_DEBUG("XUserGetANID({:08X}, {:08X}, {:08X}, {:08X})", user_index, AnId_buffer_size,
                    AnId_buffer_ptr, block);

      return X_E_SUCCESS;
    }
    case 0x000B0041: {
      assert_true(!buffer_length || buffer_length == 32);
      // 00000000 2789fecc 00000000 00000000 200491e0 00000000 200491f0 20049340
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t context_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      auto context = context_ptr ? memory_->TranslateVirtual(context_ptr) : nullptr;
      uint32_t context_id = context ? memory::load_and_swap<uint32_t>(context + 0) : 0;
      REXKRNL_DEBUG("XGIUserGetContext({:08X}, {:08X}, {:08X}))", user_index, context_ptr,
                    context_id);
      uint32_t value = 0;
      if (context) {
        memory::store_and_swap<uint32_t>(context + 4, value);
      }
      return X_E_FAIL;
    }
    case 0x000B0060: {
      assert_true(!buffer_length || buffer_length == 32);

      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t num_session_ids = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t session_ids_ptr = memory::load_and_swap<uint32_t>(buffer + 8);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t reserved1 = memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t reserved2 = memory::load_and_swap<uint32_t>(buffer + 24);
      uint32_t reserved3 = memory::load_and_swap<uint32_t>(buffer + 28);

      REXKRNL_DEBUG("XSessionSearchByIds({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                    user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                    search_results_ptr, reserved1, reserved2, reserved3);

      return X_E_SUCCESS;
    }
    case 0x000B0065: {
      assert_true(!buffer_length || buffer_length == 52);

      uint32_t proc_index = memory::load_and_swap<uint32_t>(buffer + 0);
      uint32_t user_index = memory::load_and_swap<uint32_t>(buffer + 4);
      uint32_t num_results = memory::load_and_swap<uint32_t>(buffer + 8);
      uint16_t num_weighted_properties = memory::load_and_swap<uint16_t>(buffer + 12);
      uint16_t num_weighted_contexts = memory::load_and_swap<uint16_t>(buffer + 14);
      uint32_t weighted_search_properties_ptr = memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t weighted_search_contexts_ptr = memory::load_and_swap<uint32_t>(buffer + 20);
      uint16_t num_props = memory::load_and_swap<uint16_t>(buffer + 24);
      uint16_t num_ctx = memory::load_and_swap<uint16_t>(buffer + 26);
      uint32_t non_weighted_search_properties_ptr = memory::load_and_swap<uint32_t>(buffer + 28);
      uint32_t non_weighted_search_contexts_ptr = memory::load_and_swap<uint32_t>(buffer + 32);
      uint32_t results_buffer_size = memory::load_and_swap<uint32_t>(buffer + 36);
      uint32_t search_results_ptr = memory::load_and_swap<uint32_t>(buffer + 40);
      uint32_t num_users = memory::load_and_swap<uint32_t>(buffer + 44);
      uint32_t weighted_search = memory::load_and_swap<uint32_t>(buffer + 48);

      REXKRNL_DEBUG(
          "XSessionSearchWeighted({:08X}, {:08X}, {:08X}, {}, {}, {:08X}, {:08X}, {}, {}, {:08X}, "
          "{:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
          proc_index, user_index, num_results, num_weighted_properties, num_weighted_contexts,
          weighted_search_properties_ptr, weighted_search_contexts_ptr, num_props, num_ctx,
          non_weighted_search_properties_ptr, non_weighted_search_contexts_ptr, results_buffer_size,
          search_results_ptr, num_users, weighted_search);

      return X_E_SUCCESS;
    }
    case 0x000B0071: {
      REXKRNL_DEBUG("XGI 0x000B0071, unimplemented");
      return X_E_SUCCESS;
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
