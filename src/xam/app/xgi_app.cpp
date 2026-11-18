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

#include "xam/app/xam_apps.h"
#include <rex/logging/log_category.h>
#include "xam/categories.h"
#include <rex/kernel/heap.h>
#include <rex/platform/thread.h>
#include <rex/xam/achievement_manager.h>
#include <rex/xam/app_manager.h>
#include <rex/xam/title_lifecycle.h>
#include "xam/mcla_online.h"
#include "xam/mcla_online_protocol.h"
#include "xam/stub_log.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace rex {
namespace xam {

using namespace rex::kernel;
namespace apps {
using namespace rex::kernel;

XGIApp::XGIApp(const XAMKernelInterfaces& title, rex::xam::IAchievementManager& achievements)
    : rex::xam::XAMApplet(title, 0xFB), achievements_(achievements) {}

namespace {

// XUSER_CONTEXT is {DWORD id, DWORD value} = 8 bytes.
constexpr u32 kContextSize = 8;
// XUSER_PROPERTY (Xbox 360, natural alignment):
//   +0x00 DWORD    dwPropertyId
//   +0x04 (pad)
//   +0x08 BYTE     value.type        (XUSER_DATA_TYPE; = id high nibble)
//   +0x10 union    value             (nData/i64Data/... at +16 because the
//                                      union holds an 8-byte-aligned LONGLONG)
// sizeof = 24. Emitting 16-byte-strided entries made the game's reader walk off
// the end into adjacent heap (garbage remote content mask), so the stride must
// be the full 24.
constexpr u32 kPropertySize = 24;
constexpr u32 kPropTypeOffset = 8;
constexpr u32 kPropValueOffset = 16;

// Guest pointers + counts for one search result's context/property arrays.
struct ResultData {
  u32 ctx_guest = 0;
  u16 ctx_count = 0;
  u32 prop_guest = 0;
  u16 prop_count = 0;
};

// Builds the context and property arrays a joiner reads from a search result to
// rebuild the event definition (content mask, vehicle class, max players, ...).
// Session ids are split by their high nibble: nibble 0 = XUSER_CONTEXT (game
// mode/type), non-zero = XUSER_PROPERTY (Content0..7, VehicleClass, MaxPlayers,
// ... are all property-range ids). Emitting the property-range ids as contexts
// does NOT populate the game's property blob, which is why they must go through
// pProperties. If the host carried no props (old client / empty store) we fall
// back to echoing the caller's search contexts so the session still matches.
ResultData EmitResultData(const rex::memory::GuestMembase& membase, kernel::ISystemHeap& heap,
                          const mclaonline::SessionInfo& s, u32 ctx_ptr, u16 num_ctx) {
  ResultData out;

  u16 n = s.prop_count;
  if (n > mclaonline::kMaxSessionProps) {
    n = mclaonline::kMaxSessionProps;
  }

  if (n == 0) {
    // Fallback: echo the caller's own search contexts.
    if (num_ctx > 0 && ctx_ptr) {
      out.ctx_guest = heap.SystemHeapAlloc(num_ctx * kContextSize);
      auto* block = membase.TranslateVirtual(out.ctx_guest);
      auto* src = membase.TranslateVirtual(ctx_ptr);
      std::memcpy(block, src, static_cast<size_t>(num_ctx) * kContextSize);
      out.ctx_count = num_ctx;
    }
    return out;
  }

  // Count how many go to each array.
  u16 n_ctx = 0;
  u16 n_prop = 0;
  for (u16 j = 0; j < n; ++j) {
    if ((s.props[j].id >> 28) == 0) {
      ++n_ctx;
    } else {
      ++n_prop;
    }
  }

  if (n_ctx > 0) {
    out.ctx_guest = heap.SystemHeapAlloc(n_ctx * kContextSize);
    std::memset(membase.TranslateVirtual(out.ctx_guest), 0, n_ctx * kContextSize);
  }
  if (n_prop > 0) {
    out.prop_guest = heap.SystemHeapAlloc(n_prop * kPropertySize);
    // Zero first: if the layout is still slightly off, the reader sees clean
    // zeros instead of adjacent-heap garbage.
    std::memset(membase.TranslateVirtual(out.prop_guest), 0, n_prop * kPropertySize);
  }

  u16 ci = 0;
  u16 pi = 0;
  for (u16 j = 0; j < n; ++j) {
    u32 id = s.props[j].id;
    u32 value = s.props[j].value;
    if ((id >> 28) == 0) {
      auto* e = membase.TranslateVirtual(out.ctx_guest + ci * kContextSize);
      memory::store_and_swap<u32>(e + 0, id);
      memory::store_and_swap<u32>(e + 4, value);
      ++ci;
    } else {
      auto* e = membase.TranslateVirtual(out.prop_guest + pi * kPropertySize);
      memory::store_and_swap<u32>(e + 0, id);                    // dwPropertyId
      e[kPropTypeOffset] = static_cast<u8>(id >> 28);            // value.type
      memory::store_and_swap<u32>(e + kPropValueOffset, value);  // value.nData (INT32)
      ++pi;
    }
  }
  out.ctx_count = n_ctx;
  out.prop_count = n_prop;
  return out;
}

// Writes the search results a relay session list answers an XSessionSearch / XSessionSearchEx
// with. XSESSION_SEARCHRESULT layout (per entry):
//   XSESSION_INFO info (60 bytes: sessionID[8] + XNADDR[36] + XNKEY[16])
//   DWORD dwOpenPublicSlots, dwOpenPrivateSlots,
//         dwFilledPublicSlots, dwFilledPrivateSlots   (16 bytes)
//   DWORD cProperties, cContexts                      (8 bytes)
//   ptr   pProperties, pContexts                      (8 bytes)
// Total per result: 92 bytes, after a header of dwSearchResults + pResults (8 bytes).
// Returns the number of results written.
u32 WriteRelaySearchResults(const rex::memory::GuestMembase& membase, kernel::ISystemHeap& heap,
                            const std::vector<mclaonline::SessionInfo>& sessions, u32 num_results,
                            u32 results_buffer_size, u32 search_results_ptr, u32 ctx_ptr,
                            u16 num_ctx) {
  auto* header = membase.TranslateVirtual(search_results_ptr);

  constexpr u32 kResultSize = 92;
  constexpr u32 kHeaderSize = 8;  // dwSearchResults + pResults

  u32 max_fit = 0;
  if (results_buffer_size > kHeaderSize) {
    max_fit = (results_buffer_size - kHeaderSize) / kResultSize;
  }
  const u32 count = std::min({static_cast<u32>(sessions.size()), num_results, max_fit});

  const u32 results_guest = search_results_ptr + kHeaderSize;
  memory::store_and_swap<u32>(header + 0, count);
  memory::store_and_swap<u32>(header + 4, count > 0 ? results_guest : 0);

  for (u32 i = 0; i < count; ++i) {
    const mclaonline::SessionInfo& s = sessions[i];
    auto* r = membase.TranslateVirtual(results_guest + i * kResultSize);
    // SessionInfo is a packed wire struct: read its fields into locals first, so nothing binds a
    // reference to an unaligned member.
    const u64 session_id = s.session_id;
    const u32 virtual_ip = s.virtual_ip;
    const u16 host_port = s.host_port;
    const u32 max_players = s.max_players;
    const u32 cur_players = s.cur_players;

    // XSESSION_INFO.sessionID (8 bytes)
    memory::store_and_swap<u64>(r + 0, session_id);
    // XSESSION_INFO.hostAddress.ina (4 bytes, network order)
    memory::store_and_swap<u32>(r + 8, virtual_ip);
    // XSESSION_INFO.hostAddress.inaOnline (4 bytes)
    memory::store_and_swap<u32>(r + 12, virtual_ip);
    // XSESSION_INFO.hostAddress.wPortOnline (2 bytes)
    memory::store_and_swap<u16>(r + 16, host_port);
    // XSESSION_INFO.hostAddress.abEnet (6 bytes)
    const u8 mac[6] = {0xCC, 0xCC, 0xCC, 0xCC, static_cast<u8>((virtual_ip >> 8) & 0xFF),
                       static_cast<u8>(virtual_ip & 0xFF)};
    std::memcpy(r + 18, mac, 6);
    // XSESSION_INFO.hostAddress.abOnline (20 bytes)
    std::memset(r + 24, 0, 20);
    // XSESSION_INFO.keyExchangeKey (16 bytes)
    const u8 key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    std::memcpy(r + 44, key, 16);

    // dwOpenPublicSlots
    memory::store_and_swap<u32>(r + 60, max_players - cur_players);
    // dwOpenPrivateSlots
    memory::store_and_swap<u32>(r + 64, 0);
    // dwFilledPublicSlots
    memory::store_and_swap<u32>(r + 68, cur_players);
    // dwFilledPrivateSlots
    memory::store_and_swap<u32>(r + 72, 0);
    // Host's real session properties (event definition), split into contexts + properties by id
    // nibble; fallback echoes the caller's search contexts. See EmitResultData.
    const ResultData rd = EmitResultData(membase, heap, s, ctx_ptr, num_ctx);
    memory::store_and_swap<u32>(r + 76, rd.prop_count);  // cProperties
    memory::store_and_swap<u32>(r + 80, rd.ctx_count);   // cContexts
    memory::store_and_swap<u32>(r + 84, rd.prop_guest);  // pProperties
    memory::store_and_swap<u32>(r + 88, rd.ctx_guest);   // pContexts
  }
  return count;
}

// XSESSION_CREATE flags. Only the host bit matters here: on real XAM,
// XSessionCreate *fills in* XSESSION_INFO when the caller is hosting, and
// *consumes* the caller's XSESSION_INFO (taken from a search result or an
// invite) when the caller is joining. Overwriting it on a joiner points the
// guest's rage netSession at its own address, so it elects itself session owner
// (netSession+0x1190 == its own peerId, sub_82481688 true), which in turn makes
// mcNetworkClock start in SERVER mode instead of syncing to the host. Race
// challenges carry an absolute deadline on the sender's clock, so with two
// server clocks the receiver's deadline test (sub_82293EB0, 0x82282F74) can
// never pass and no invite popup is ever raised.
constexpr u32 kSessionCreateHost = 0x00000001;

// XSESSION_INFO is 60 bytes: sessionID(8) + XNADDR hostAddress(36) +
// keyExchangeKey(16).
constexpr u32 kSessionInfoSize = 60;

// The XSESSION_INFO of the session we are currently in, so XSessionGetDetails
// reports the real host rather than always describing ourselves.
std::mutex g_session_info_mutex;
u8 g_session_info[kSessionInfoSize] = {};
bool g_session_info_valid = false;
bool g_session_is_host = false;

void RememberSessionInfo(const u8* raw, bool is_host) {
  std::lock_guard<std::mutex> lock(g_session_info_mutex);
  std::memcpy(g_session_info, raw, kSessionInfoSize);
  g_session_info_valid = true;
  g_session_is_host = is_host;
}

bool RecallSessionInfo(u8* out, bool* is_host) {
  std::lock_guard<std::mutex> lock(g_session_info_mutex);
  if (!g_session_info_valid) {
    return false;
  }
  std::memcpy(out, g_session_info, kSessionInfoSize);
  *is_host = g_session_is_host;
  return true;
}

}  // namespace

kernel::X_HRESULT XGIApp::DispatchMessageSync(u32 message, u32 buffer_ptr, u32 buffer_length) {
  switch (message) {
    case 0x000B0006: {
      REX_ASSERT(!buffer_length || buffer_length == 24);
      struct {
        be_u32 user_index;
        u8 pad_04[12];
        be_u32 context_id;
        be_u32 context_value;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 24);
      u32 user_index = args->user_index;
      u32 context_id = args->context_id;
      u32 context_value = args->context_value;
      REXXAM_DEBUG("XGIUserSetContextEx({:08X}, {:08X}, {:08X})", user_index, context_id,
                   context_value);
      // Capture the real value at set time so the advertised session carries the event
      // definition (search result properties). A one shot store read at XNetQosListen runs before
      // the game sets these, so it captured defaults.
      if (OnlineClient::Enabled()) {
        REXXAM_INFO("XGIUserSetContextEx(user={:08X}, ctx={:08X}, val={:08X})", user_index,
                    context_id, context_value);
        OnlineClient::Get().UpdateSessionProp(context_id, context_value);
      }
      return kernel::X_E_SUCCESS;
    }
    case 0x000B0007: {
      struct {
        be_u32 user_index;
        u8 pad_04[12];
        be_u32 property_id;
        be_u32 value_size;
        be_u32 value_ptr;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 28);
      u32 user_index = args->user_index;
      u32 property_id = args->property_id;
      u32 value_size = args->value_size;
      u32 value_ptr = args->value_ptr;
      REXXAM_DEBUG("XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})", user_index, property_id,
                   value_size, value_ptr);
      // Capture 32 bit property values as the game sets them (see SetContextEx). 64 bit
      // properties (e.g. HostUID) don't gate the event; skip them.
      if (OnlineClient::Enabled()) {
        REXXAM_INFO("XGIUserSetPropertyEx(user={:08X}, prop={:08X}, size={}, ptr={:08X})",
                    user_index, property_id, value_size, value_ptr);
        if (value_size == 4 && value_ptr) {
          const u32 value = *membase_.TranslateVirtual<be_u32*>(value_ptr);
          OnlineClient::Get().UpdateSessionProp(property_id, value);
        }
      }
      return kernel::X_E_SUCCESS;
    }
    case 0x000B0008: {
      struct {
        be_u32 count;
        be_u32 achievements_ptr;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 8);
      u32 raw0 = buffer_length >= 4 ? u32{args->count} : 0;
      u32 raw4 = buffer_length >= 8 ? u32{args->achievements_ptr} : 0;
      REXXAM_DEBUG("XGIUserWriteAchievements called: buf_len={} raw[0]={:08X} raw[4]={:08X}",
                   buffer_length, raw0, raw4);

      REX_ASSERT(!buffer_length || buffer_length == 8);
      u32 achievement_count = raw0;
      u32 achievements_ptr = raw4;

      struct Entry {
        be_u32 user_index;
        be_u32 id;
      };
      REX_STATIC_ASSERT_SIZE(Entry, 8);
      static_assert(offsetof(Entry, id) == 4);
      constexpr u32 kMaxAchievements = 1000;

      if (achievements_ptr && achievement_count > 0) {
        if (achievement_count > kMaxAchievements) {
          REXXAM_WARN("XGIUserWriteAchievements: count={} unreasonable, ignoring",
                      achievement_count);
          return kernel::X_E_FAIL;
        }
        u32 span_end = achievements_ptr + achievement_count * sizeof(Entry) - 1;
        if (!title_.heap.IsAddressMapped(achievements_ptr) ||
            !title_.heap.IsAddressMapped(span_end)) {
          REXXAM_WARN("XGIUserWriteAchievements: ptr {:08X} OOB", achievements_ptr);
          return kernel::X_E_FAIL;
        }
        const auto* entries = membase_.TranslateVirtual<const Entry*>(achievements_ptr);
        for (u32 i = 0; i < achievement_count; ++i) {
          u32 id = entries[i].id;
          REXXAM_DEBUG("XGIUserWriteAchievements: id={} ({})", id, i);
          (void)achievements_.UnlockAchievement(id, rex::xam::AchievementNotification::kShow);
        }
      } else {
        REXXAM_DEBUG("XGIUserWriteAchievements: skipped (count={} ptr={:08X})", achievement_count,
                     achievements_ptr);
      }
      return kernel::X_E_SUCCESS;
    }
    case 0x000B0010: {
      REX_ASSERT(!buffer_length || buffer_length == 28);
      struct {
        be_u32 session_ptr;
        be_u32 flags;
        be_u32 num_slots_public;
        be_u32 num_slots_private;
        be_u32 user_xuid;
        be_u32 session_info_ptr;
        be_u32 nonce_ptr;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 28);
      u32 session_ptr = args->session_ptr;
      u32 flags = args->flags;
      u32 num_slots_public = args->num_slots_public;
      u32 num_slots_private = args->num_slots_private;
      u32 user_xuid = args->user_xuid;
      u32 session_info_ptr = args->session_info_ptr;
      u32 nonce_ptr = args->nonce_ptr;

      REXXAM_DEBUG(
          "XGISessionCreate({:08X}, {:08X}, {}, {}, {:08X}, {:08X}, "
          "{:08X})",
          session_ptr, flags, num_slots_public, num_slots_private, user_xuid, session_info_ptr,
          nonce_ptr);

      if (session_ptr) {
        memory::store_and_swap<u32>(membase_.TranslateVirtual(session_ptr), 0xDEADF00D);
      }

      const bool creating_as_host = (flags & kSessionCreateHost) != 0;

      // Unconditional so a run where the guest never calls this at all is distinguishable from
      // one where it calls it with no session info.
      REXXAM_INFO("[online-diag] XSessionCreate flags={:08X} host_bit={} info_ptr={:08X}", flags,
                  creating_as_host ? 1 : 0, session_info_ptr);

      // A joiner passes in the XSESSION_INFO it got from XSessionSearchEx (or from an invite);
      // that buffer already holds the *host's* address and session id, and XAM leaves it alone.
      // Only a host has its session info filled in here. Synthesising it unconditionally was
      // making both machines describe themselves as the host - see kSessionCreateHost.
      if (session_info_ptr && !creating_as_host) {
        auto session_info = membase_.TranslateVirtual(session_info_ptr);
        const u32 host_ip = memory::load_and_swap<u32>(session_info + 8);
        // Only treat this as a real join if the caller actually handed us someone else's host
        // address. The game also creates flagless sessions that carry no host info (offline/local
        // types), and those must not silence our own lobby advertisement.
        const u32 my_vip = OnlineClient::Get().virtual_ip();
        const bool joining_remote = host_ip != 0 && host_ip != my_vip;
        if (joining_remote) {
          RememberSessionInfo(session_info, false);
          OnlineClient::Get().set_advertise_as_host(false);
        }
        REXXAM_INFO(
            "[online-diag] XSessionCreate flags={:08X} host_bit=0 host=10.0.0.{} port={} "
            "sid={:016X} joining_remote={}",
            flags, host_ip & 0xFF, memory::load_and_swap<u16>(session_info + 16),
            memory::load_and_swap<u64>(session_info + 0), joining_remote ? 1 : 0);
      }

      if (session_info_ptr && creating_as_host) {
        auto session_info = membase_.TranslateVirtual(session_info_ptr);

        // CHECK: v0.10 + MCLA start the relay client here even with mcla_online.enable off (the
        // session id below comes from it, and offline System Link may rely on that id). Kept.
        auto& client = OnlineClient::Get();
        client.EnsureStarted();
        const u64 sid = client.session_id();
        u32 my_ip = 0x7F000001;  // localhost fallback
        u32 vip = 0;

        if (OnlineClient::Enabled()) {
          if (!client.connection_checked()) {
            const u32 elapsed = OnlineClient::NowMs() - client.startup_time_ms();
            if (elapsed < 250) {
              const u32 remaining = 250 - elapsed;
              for (u32 i = 0; i < remaining / 5 && !client.connection_checked(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
              }
            }
            if (!client.connection_checked()) {
              client.set_connection_checked(true);
            }
          }
          if (client.server_online()) {
            vip = client.virtual_ip();
          }
        }

        if (vip != 0) {
          my_ip = vip;
        }

        // Write sessionID (u64)
        memory::store_and_swap<u64>(session_info + 0, sid);

        // Write hostAddress.ina (u32)
        memory::store_and_swap<u32>(session_info + 8, my_ip);
        // Write hostAddress.inaOnline (u32)
        memory::store_and_swap<u32>(session_info + 12, my_ip);
        // Write hostAddress.wPortOnline (u16)
        u16 game_port = client.game_port();
        if (game_port == 0) {
          game_port = 1000;
        }
        memory::store_and_swap<u16>(session_info + 16, game_port);

        // Write hostAddress.abEnet (6 bytes)
        const u8 mac[6] = {0xCC, 0xCC, 0xCC, 0xCC, static_cast<u8>((my_ip >> 8) & 0xFF),
                           static_cast<u8>(my_ip & 0xFF)};
        std::memcpy(session_info + 18, mac, 6);

        // Write hostAddress.abOnline (20 bytes)
        std::memset(session_info + 24, 0, 20);

        // Write keyExchangeKey (16 bytes)
        const u8 key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
        std::memcpy(session_info + 44, key, 16);

        RememberSessionInfo(session_info, true);
        client.set_advertise_as_host(true);
        REXXAM_INFO("[online-diag] XSessionCreate HOST flags={:08X} ip=10.0.0.{} port={}", flags,
                    my_ip & 0xFF, game_port);
      }

      if (nonce_ptr) {
        auto nonce = membase_.TranslateVirtual(nonce_ptr);
        const u64 nonce_val = 0x1122334455667788ull;
        memory::store_and_swap<u64>(nonce, nonce_val);
      }

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0011: {
      REX_ASSERT(!buffer_length || buffer_length == 16);

#pragma pack(push, 4)
      struct {
        be_u32 obj_ptr;
        be_u32 flags;
        be_u64 session_nonce;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
#pragma pack(pop)
      REX_STATIC_ASSERT_SIZE(decltype(*args), 16);
      u32 obj_ptr = args->obj_ptr;
      u32 flags = args->flags;
      u64 session_nonce = args->session_nonce;

      REXXAM_DEBUG("XGISessionDelete({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0012: {
      REX_ASSERT(!buffer_length || buffer_length == 20);
      struct {
        be_u32 session_ptr;
        be_u32 user_count;
        be_u32 xuid_array_ptr;
        be_u32 user_index_array;
        be_u32 private_slots_array;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 20);
      u32 session_ptr = args->session_ptr;
      u32 user_count = args->user_count;
      u32 xuid_array_ptr = args->xuid_array_ptr;
      u32 user_index_array = args->user_index_array;
      u32 private_slots_array = args->private_slots_array;
      bool is_local = xuid_array_ptr == 0;

      REXXAM_DEBUG("{}({:08X}, {}, {}, {:08X}, {:08X})",
                   is_local ? "XGISessionJoinLocal" : "XGISessionJoinRemote", session_ptr,
                   user_count, xuid_array_ptr, user_index_array, private_slots_array);
      return kernel::X_E_SUCCESS;
    }
    case 0x000B0013: {
      // XSessionJoinRemote(session, xuid_count, pXuids, pfPrivateSlots). The actual join step:
      // without it Quick Cruise search finds the host but the join call fails, so the game falls
      // back to creating its own session. We track players over the game's own relayed
      // networking, so accepting the join is enough here; the arbitration/player list is not
      // emulated.
      REX_ASSERT(!buffer_length || buffer_length == 20);
      struct {
        be_u32 session_ptr;
        be_u32 xuid_count;
        be_u32 unk_0;
        be_u32 xuid_array;
        be_u32 private_slots_array;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 20);
      u32 session_ptr = args->session_ptr;
      u32 xuid_count = args->xuid_count;
      u32 unk_0 = args->unk_0;
      u32 xuid_array = args->xuid_array;
      u32 private_slots_array = args->private_slots_array;

      REXXAM_INFO("XGISessionJoinRemote({:08X}, {}, {}, {:08X}, {:08X})", session_ptr, xuid_count,
                  unk_0, xuid_array, private_slots_array);
      return kernel::X_E_SUCCESS;
    }
    case 0x000B0014: {
      REX_ASSERT(!buffer_length || buffer_length == 16);

#pragma pack(push, 4)
      struct {
        be_u32 obj_ptr;
        be_u32 flags;
        be_u64 session_nonce;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
#pragma pack(pop)
      REX_STATIC_ASSERT_SIZE(decltype(*args), 16);
      u32 obj_ptr = args->obj_ptr;
      u32 flags = args->flags;
      u64 session_nonce = args->session_nonce;

      REXXAM_DEBUG("XSessionStart({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return kernel::X_STATUS_SUCCESS;
    }
    case 0x000B0015: {
      REX_ASSERT(!buffer_length || buffer_length == 16);

#pragma pack(push, 4)
      struct {
        be_u32 obj_ptr;
        be_u32 flags;
        be_u64 session_nonce;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
#pragma pack(pop)
      REX_STATIC_ASSERT_SIZE(decltype(*args), 16);
      u32 obj_ptr = args->obj_ptr;
      u32 flags = args->flags;
      u64 session_nonce = args->session_nonce;

      REXXAM_DEBUG("XSessionEnd({:08X}, {:08X}, {:016X})", obj_ptr, flags, session_nonce);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0016: {
      REX_ASSERT(!buffer_length || buffer_length == 32);

      struct {
        be_u32 proc_index;
        be_u32 user_index;
        be_u32 num_results;
        be_u16 num_props;
        be_u16 num_ctx;
        be_u32 props_ptr;
        be_u32 ctx_ptr;
        be_u32 results_buffer_size;
        be_u32 search_results_ptr;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 32);
      u32 proc_index = args->proc_index;
      u32 user_index = args->user_index;
      u32 num_results = args->num_results;
      u16 num_props = args->num_props;
      u16 num_ctx = args->num_ctx;
      u32 props_ptr = args->props_ptr;
      u32 ctx_ptr = args->ctx_ptr;
      u32 results_buffer_size = args->results_buffer_size;
      u32 search_results_ptr = args->search_results_ptr;

      REXXAM_DEBUG("XSessionSearch({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X})", proc_index,
                   user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                   results_buffer_size, search_results_ptr);

      if (OnlineClient::Enabled()) {
        REXXAM_INFO("XSessionSearch({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X})", proc_index,
                    user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                    results_buffer_size, search_results_ptr);
        if (ctx_ptr && num_ctx > 0) {
          auto* ctx = membase_.TranslateVirtual(ctx_ptr);
          for (u16 ci = 0; ci < num_ctx; ++ci) {
            const u32 ctx_id = memory::load_and_swap<u32>(ctx + ci * 8);
            const u32 ctx_val = memory::load_and_swap<u32>(ctx + ci * 8 + 4);
            REXXAM_INFO("  search ctx[{}]: id={:08X} val={:08X}", ci, ctx_id, ctx_val);
          }
        }
        if (props_ptr && num_props > 0) {
          auto* props = membase_.TranslateVirtual(props_ptr);
          for (u16 pi = 0; pi < num_props; ++pi) {
            const u32 prop_id = memory::load_and_swap<u32>(props + pi * 16);
            REXXAM_INFO("  search prop[{}]: id={:08X}", pi, prop_id);
          }
        }
      }

      if (search_results_ptr && OnlineClient::Enabled()) {
        const auto sessions = OnlineClient::Get().QuerySessions(500);
        const u32 count = WriteRelaySearchResults(membase_, title_.heap, sessions, num_results,
                                                  results_buffer_size, search_results_ptr, ctx_ptr,
                                                  num_ctx);
        const u16 first_props = count > 0 ? sessions[0].prop_count : u16{0};
        REXXAM_INFO("XSessionSearch: returning {} sessions from relay ({} host props on first)",
                    count, first_props);
      } else if (search_results_ptr) {
        auto header = membase_.TranslateVirtual(search_results_ptr);
        memory::store_and_swap<u32>(header + 0, 0);
        memory::store_and_swap<u32>(header + 4, 0);
      }
      return kernel::X_E_SUCCESS;
    }
    case 0x000B0018: {
      REX_ASSERT(!buffer_length || buffer_length == 16);

      struct {
        be_u32 obj_ptr;
        be_u32 flags;
        be_u32 max_public_slots;
        be_u16 max_private_slots;
        u8 pad_0e[2];
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 16);
      u32 obj_ptr = args->obj_ptr;
      u32 flags = args->flags;
      u32 max_public_slots = args->max_public_slots;
      u16 max_private_slots = args->max_private_slots;

      REXXAM_DEBUG("XSessionModify({:08X}, {:08X}, {:08X}, {:08X})", obj_ptr, flags,
                   max_public_slots, max_private_slots);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B001C: {
      REX_ASSERT(!buffer_length || buffer_length == 36);

      struct {
        be_u32 proc_index;
        be_u32 user_index;
        be_u32 num_results;
        be_u16 num_props;
        be_u16 num_ctx;
        be_u32 props_ptr;
        be_u32 ctx_ptr;
        be_u32 results_buffer_size;
        be_u32 search_results_ptr;
        be_u32 num_users;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 36);
      u32 proc_index = args->proc_index;
      u32 user_index = args->user_index;
      u32 num_results = args->num_results;
      u16 num_props = args->num_props;
      u16 num_ctx = args->num_ctx;
      u32 props_ptr = args->props_ptr;
      u32 ctx_ptr = args->ctx_ptr;
      u32 results_buffer_size = args->results_buffer_size;
      u32 search_results_ptr = args->search_results_ptr;
      u32 num_users = args->num_users;

      REXXAM_DEBUG("XSessionSearchEx({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X}, {})",
                   proc_index, user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                   results_buffer_size, search_results_ptr, num_users);

      if (OnlineClient::Enabled()) {
        REXXAM_INFO("XSessionSearchEx({}, {}, {}, {}, {}, {:08X}, {:08X}, {}, {:08X}, {})",
                    proc_index, user_index, num_results, num_props, num_ctx, props_ptr, ctx_ptr,
                    results_buffer_size, search_results_ptr, num_users);
        if (ctx_ptr && num_ctx > 0) {
          auto* ctx = membase_.TranslateVirtual(ctx_ptr);
          for (u16 ci = 0; ci < num_ctx; ++ci) {
            const u32 ctx_id = memory::load_and_swap<u32>(ctx + static_cast<size_t>(ci) * 8);
            const u32 ctx_val = memory::load_and_swap<u32>(ctx + static_cast<size_t>(ci) * 8 + 4);
            REXXAM_INFO("  searchEx ctx[{}]: id={:08X} val={:08X}", ci, ctx_id, ctx_val);
          }
        }
      }

      if (search_results_ptr && OnlineClient::Enabled()) {
        // Emit the host's real session properties (event definition) so the joiner rebuilds
        // content mask / vehicle class / etc. Split into contexts + properties by id nibble; see
        // EmitResultData.
        const auto sessions = OnlineClient::Get().QuerySessions(500);
        const u32 count = WriteRelaySearchResults(membase_, title_.heap, sessions, num_results,
                                                  results_buffer_size, search_results_ptr, ctx_ptr,
                                                  num_ctx);
        const u16 first_props = count > 0 ? sessions[0].prop_count : u16{0};
        REXXAM_INFO("XSessionSearchEx: returning {} sessions from relay ({} host props on first)",
                    count, first_props);
      } else if (search_results_ptr) {
        auto header = membase_.TranslateVirtual(search_results_ptr);
        memory::store_and_swap<u32>(header + 0, 0);
        memory::store_and_swap<u32>(header + 4, 0);
      }

      return kernel::X_E_SUCCESS;
    }
    case 0x000B001D: {
      REX_ASSERT(!buffer_length || buffer_length == 24);

      struct {
        be_u32 obj_ptr;
        be_u32 details_buffer_size;
        be_u32 session_details_ptr;
        be_u32 reserved1;
        be_u32 reserved2;
        be_u32 reserved3;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 24);
      u32 obj_ptr = args->obj_ptr;
      u32 details_buffer_size = args->details_buffer_size;
      u32 session_details_ptr = args->session_details_ptr;
      u32 reserved1 = args->reserved1;
      u32 reserved2 = args->reserved2;
      u32 reserved3 = args->reserved3;

      REXXAM_DEBUG("XSessionGetDetails({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr,
                   details_buffer_size, session_details_ptr, reserved1, reserved2, reserved3);

      if (session_details_ptr) {
        auto details = membase_.TranslateVirtual(session_details_ptr);

        auto& client = OnlineClient::Get();
        client.EnsureStarted();
        const u64 sid = client.session_id();
        u32 my_ip = 0x7F000001;  // localhost fallback
        u32 vip = client.virtual_ip();
        for (int i = 0; i < 200 && vip == 0; ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          vip = client.virtual_ip();
        }
        if (vip != 0) {
          my_ip = vip;
        }

        // dwUserIndexHost (DWORD)
        memory::store_and_swap<u32>(details + 0, 0);
        // dwGameType (DWORD)
        memory::store_and_swap<u32>(details + 4, 0);
        // dwGameMode (DWORD)
        memory::store_and_swap<u32>(details + 8, 0);
        // dwFlags (DWORD)
        memory::store_and_swap<u32>(details + 12, 0);
        // dwMaxPublicSlots (DWORD)
        memory::store_and_swap<u32>(details + 16, 8);
        // dwMaxPrivateSlots (DWORD)
        memory::store_and_swap<u32>(details + 20, 0);
        // dwAvailablePublicSlots (DWORD)
        memory::store_and_swap<u32>(details + 24, 8);
        // dwAvailablePrivateSlots (DWORD)
        memory::store_and_swap<u32>(details + 28, 0);
        // dwActualMemberCount (DWORD)
        memory::store_and_swap<u32>(details + 32, 1);
        // dwReturnedMemberCount (DWORD)
        memory::store_and_swap<u32>(details + 36, 1);
        // eState (XSESSION_STATE, lobby = 1)
        memory::store_and_swap<u32>(details + 40, 1);
        // qwNonce (ULONGLONG)
        memory::store_and_swap<u64>(details + 44, 0x1122334455667788ull);

        // sessionInfo (XSESSION_INFO) - Offset 52.
        // Replay the info the session was created/joined with. Describing ourselves here on a
        // guest is what let the joiner keep pointing its netSession at its own address (see
        // kSessionCreateHost).
        u8 remembered[kSessionInfoSize];
        bool remembered_is_host = false;
        if (RecallSessionInfo(remembered, &remembered_is_host)) {
          std::memcpy(details + 52, remembered, kSessionInfoSize);
        } else {
          // Write sessionID (u64)
          memory::store_and_swap<u64>(details + 52, sid);

          // Write hostAddress.ina (u32)
          memory::store_and_swap<u32>(details + 60, my_ip);
          // Write hostAddress.inaOnline (u32)
          memory::store_and_swap<u32>(details + 64, my_ip);
          // Write hostAddress.wPortOnline (u16)
          u16 game_port = client.game_port();
          if (game_port == 0) {
            game_port = 1000;
          }
          memory::store_and_swap<u16>(details + 68, game_port);

          // Write hostAddress.abEnet (6 bytes)
          const u8 mac[6] = {0xCC, 0xCC, 0xCC, 0xCC, static_cast<u8>((my_ip >> 8) & 0xFF),
                             static_cast<u8>(my_ip & 0xFF)};
          std::memcpy(details + 70, mac, 6);

          // Write hostAddress.abOnline (20 bytes)
          std::memset(details + 76, 0, 20);

          // Write keyExchangeKey (16 bytes)
          const u8 key[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
          std::memcpy(details + 96, key, 16);
        }

        // xnkidArbitration (XNKID) - Offset 112 (8 bytes)
        std::memset(details + 112, 0, 8);

        // pSessionMembers (XSESSION_MEMBER*) - Offset 120 (4 bytes in 32-bit guest)
        memory::store_and_swap<u32>(details + 120, 0);
      }

      return kernel::X_E_SUCCESS;
    }
    case 0x000B001E: {
      REX_ASSERT(!buffer_length || buffer_length == 24);

      struct {
        be_u32 obj_ptr;
        be_u32 session_info_ptr;
        be_u32 user_index;
        be_u32 reserved1;
        be_u32 reserved2;
        be_u32 reserved3;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 24);
      u32 obj_ptr = args->obj_ptr;
      u32 session_info_ptr = args->session_info_ptr;
      u32 user_index = args->user_index;
      u32 reserved1 = args->reserved1;
      u32 reserved2 = args->reserved2;
      u32 reserved3 = args->reserved3;

      REXXAM_DEBUG("XSessionMigrateHost({:08X}, {:08X}, {}, {}, {}, {})", obj_ptr, session_info_ptr,
                   user_index, reserved1, reserved2, reserved3);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0019: {
      REX_ASSERT(!buffer_length || buffer_length == 8);

      struct {
        be_u32 user_index;
        be_u32 session_info_ptr;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 8);
      u32 user_index = args->user_index;
      u32 session_info_ptr = args->session_info_ptr;

      REX_XAM_STUB("XSessionGetInvitationData({}, {:08X})", user_index, session_info_ptr);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B001A: {
      REX_ASSERT(!buffer_length || buffer_length == 28);

#pragma pack(push, 4)
      struct {
        be_u32 obj_ptr;
        be_u32 flags;
        be_u64 session_nonce;
        be_u32 session_duration_sec;
        be_u32 results_buffer_size;
        be_u32 results_ptr;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
#pragma pack(pop)
      REX_STATIC_ASSERT_SIZE(decltype(*args), 28);
      u32 obj_ptr = args->obj_ptr;
      u32 flags = args->flags;
      u64 session_nonce = args->session_nonce;
      u32 session_duration_sec = args->session_duration_sec;
      u32 results_buffer_size = args->results_buffer_size;
      u32 results_ptr = args->results_ptr;

      REXXAM_DEBUG("XSessionArbitrationRegister({:08X}, {:08X}, {:016X}, {:08X}, {:08X}, {:08X})",
                   obj_ptr, flags, session_nonce, session_duration_sec, results_buffer_size,
                   results_ptr);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B001B: {
      REX_ASSERT(!buffer_length || buffer_length == 32);

      struct {
        be_u32 user_index;
        be_u32 num_session_ids;
        be_u32 session_ids_ptr;
        be_u32 results_buffer_size;
        be_u32 search_results_ptr;
        be_u32 reserved1;
        be_u32 reserved2;
        be_u32 reserved3;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 32);
      u32 user_index = args->user_index;
      u32 num_session_ids = args->num_session_ids;
      u32 session_ids_ptr = args->session_ids_ptr;
      u32 results_buffer_size = args->results_buffer_size;
      u32 search_results_ptr = args->search_results_ptr;
      u32 reserved1 = args->reserved1;
      u32 reserved2 = args->reserved2;
      u32 reserved3 = args->reserved3;

      REXXAM_DEBUG("XSessionSearchByID({}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})", user_index,
                   num_session_ids, session_ids_ptr, results_buffer_size, search_results_ptr,
                   reserved1, reserved2, reserved3);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B001F: {
      REX_ASSERT(!buffer_length || buffer_length == 24);

      struct {
        be_u32 obj_ptr;
        be_u32 array_count;
        be_u32 xuid_array_ptr;
        be_u32 reserved1;
        be_u32 reserved2;
        be_u32 reserved3;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 24);
      u32 obj_ptr = args->obj_ptr;
      u32 array_count = args->array_count;
      u32 xuid_array_ptr = args->xuid_array_ptr;
      u32 reserved1 = args->reserved1;
      u32 reserved2 = args->reserved2;
      u32 reserved3 = args->reserved3;

      REXXAM_DEBUG("XSessionModifySkill({:08X}, {}, {:08X}, {}, {}, {})", obj_ptr, array_count,
                   xuid_array_ptr, reserved1, reserved2, reserved3);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0020: {
      REX_ASSERT(!buffer_length || buffer_length == 8);

      struct {
        be_u32 user_index;
        be_u32 view_id;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 8);
      u32 user_index = args->user_index;
      u32 view_id = args->view_id;

      REXXAM_DEBUG("XUserResetStatsView({:08X}, {})", user_index, view_id);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0021: {
      REX_ASSERT(!buffer_length || buffer_length == 28);

      struct {
        be_u32 title_id;
        be_u32 xuids_count;
        be_u32 xuids_ptr;
        be_u32 specs_count;
        be_u32 specs_ptr;
        be_u32 results_size;
        be_u32 results_ptr;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 28);
      u32 title_id = args->title_id;
      u32 xuids_count = args->xuids_count;
      u32 xuids_ptr = args->xuids_ptr;
      u32 specs_count = args->specs_count;
      u32 specs_ptr = args->specs_ptr;
      u32 results_size = args->results_size;
      u32 results_ptr = args->results_ptr;

      REXXAM_DEBUG("XUserReadStats({}, {}, {:08X}, {}, {:08X}, {}, {:08X})", title_id, xuids_count,
                   xuids_ptr, specs_count, specs_ptr, results_size, results_ptr);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0025: {
      REX_ASSERT(!buffer_length || buffer_length == 20);

#pragma pack(push, 4)
      struct {
        be_u32 obj_ptr;
        be_u64 xuid;
        be_u32 num_views;
        be_u32 views_ptr;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
#pragma pack(pop)
      REX_STATIC_ASSERT_SIZE(decltype(*args), 20);
      u32 obj_ptr = args->obj_ptr;
      u64 xuid = args->xuid;
      u32 num_views = args->num_views;
      u32 views_ptr = args->views_ptr;

      REXXAM_DEBUG("XSessionWriteStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                   views_ptr);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0026: {
      REX_ASSERT(!buffer_length || buffer_length == 20);

#pragma pack(push, 4)
      struct {
        be_u32 obj_ptr;
        be_u64 xuid;
        be_u32 num_views;
        be_u32 views_ptr;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
#pragma pack(pop)
      REX_STATIC_ASSERT_SIZE(decltype(*args), 20);
      u32 obj_ptr = args->obj_ptr;
      u64 xuid = args->xuid;
      u32 num_views = args->num_views;
      u32 views_ptr = args->views_ptr;

      REXXAM_DEBUG("XSessionFlushStats({:08X}, {:016X}, {:08X}, {:08X})", obj_ptr, xuid, num_views,
                   views_ptr);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0036: {
      // The message carries no buffer.
      REX_XAM_STUB("XInvalidateGamerTileCache");
      return kernel::X_E_FAIL;
    }
    case 0x000B003D: {
      REX_ASSERT(!buffer_length || buffer_length == 16);

      struct {
        be_u32 user_index;
        be_u32 anid_buffer_size;
        be_u32 anid_buffer_ptr;
        be_u32 block;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 16);
      u32 user_index = args->user_index;
      u32 anid_buffer_size = args->anid_buffer_size;
      u32 anid_buffer_ptr = args->anid_buffer_ptr;
      u32 block = args->block;

      REXXAM_DEBUG("XUserGetANID({:08X}, {:08X}, {:08X}, {:08X})", user_index, anid_buffer_size,
                   anid_buffer_ptr, block);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0041: {
      REX_ASSERT(!buffer_length || buffer_length == 32);
      struct {
        be_u32 user_index;
        u8 pad_04[12];
        be_u32 context_ptr;
        u8 pad_14[12];
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 32);
      struct Context {
        be_u32 id;
        be_u32 value;
      };
      REX_STATIC_ASSERT_SIZE(Context, 8);
      u32 user_index = args->user_index;
      u32 context_ptr = args->context_ptr;
      auto* context = context_ptr ? membase_.TranslateVirtual<Context*>(context_ptr) : nullptr;
      u32 context_id = context ? u32{context->id} : 0;
      REXXAM_DEBUG("XGIUserGetContext({:08X}, {:08X}, {:08X}))", user_index, context_ptr,
                   context_id);
      u32 value = 0;
      if (context) {
        context->value = value;
      }
      return kernel::X_E_FAIL;
    }
    case 0x000B0060: {
      REX_ASSERT(!buffer_length || buffer_length == 32);

      struct {
        be_u32 user_index;
        be_u32 num_session_ids;
        be_u32 session_ids_ptr;
        be_u32 results_buffer_size;
        be_u32 search_results_ptr;
        be_u32 reserved1;
        be_u32 reserved2;
        be_u32 reserved3;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 32);
      u32 user_index = args->user_index;
      u32 num_session_ids = args->num_session_ids;
      u32 session_ids_ptr = args->session_ids_ptr;
      u32 results_buffer_size = args->results_buffer_size;
      u32 search_results_ptr = args->search_results_ptr;
      u32 reserved1 = args->reserved1;
      u32 reserved2 = args->reserved2;
      u32 reserved3 = args->reserved3;

      REXXAM_DEBUG("XSessionSearchByIds({:08X}, {:08X}, {:08X}, {:08X}, {:08X}, {}, {}, {})",
                   user_index, num_session_ids, session_ids_ptr, results_buffer_size,
                   search_results_ptr, reserved1, reserved2, reserved3);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0065: {
      REX_ASSERT(!buffer_length || buffer_length == 52);

      struct {
        be_u32 proc_index;
        be_u32 user_index;
        be_u32 num_results;
        be_u16 num_weighted_properties;
        be_u16 num_weighted_contexts;
        be_u32 weighted_search_properties_ptr;
        be_u32 weighted_search_contexts_ptr;
        be_u16 num_props;
        be_u16 num_ctx;
        be_u32 non_weighted_search_properties_ptr;
        be_u32 non_weighted_search_contexts_ptr;
        be_u32 results_buffer_size;
        be_u32 search_results_ptr;
        be_u32 num_users;
        be_u32 weighted_search;
      }* args = membase_.TranslateVirtual<decltype(args)>(buffer_ptr);
      REX_STATIC_ASSERT_SIZE(decltype(*args), 52);
      u32 proc_index = args->proc_index;
      u32 user_index = args->user_index;
      u32 num_results = args->num_results;
      u16 num_weighted_properties = args->num_weighted_properties;
      u16 num_weighted_contexts = args->num_weighted_contexts;
      u32 weighted_search_properties_ptr = args->weighted_search_properties_ptr;
      u32 weighted_search_contexts_ptr = args->weighted_search_contexts_ptr;
      u16 num_props = args->num_props;
      u16 num_ctx = args->num_ctx;
      u32 non_weighted_search_properties_ptr = args->non_weighted_search_properties_ptr;
      u32 non_weighted_search_contexts_ptr = args->non_weighted_search_contexts_ptr;
      u32 results_buffer_size = args->results_buffer_size;
      u32 search_results_ptr = args->search_results_ptr;
      u32 num_users = args->num_users;
      u32 weighted_search = args->weighted_search;

      REXXAM_DEBUG(
          "XSessionSearchWeighted({:08X}, {:08X}, {:08X}, {}, {}, {:08X}, {:08X}, {}, {}, {:08X}, "
          "{:08X}, {:08X}, {:08X}, {:08X}, {:08X})",
          proc_index, user_index, num_results, num_weighted_properties, num_weighted_contexts,
          weighted_search_properties_ptr, weighted_search_contexts_ptr, num_props, num_ctx,
          non_weighted_search_properties_ptr, non_weighted_search_contexts_ptr, results_buffer_size,
          search_results_ptr, num_users, weighted_search);

      return kernel::X_E_SUCCESS;
    }
    case 0x000B0071: {
      REX_XAM_STUB("XGI 0x000B0071");
      return kernel::X_E_SUCCESS;
    }
    default:
      break;
  }
  REXXAM_FAIL(kernel::X_E_FAIL,
              "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
              "arg2={:08X}",
              GetAppID(), message, buffer_ptr, buffer_length);
}

}  // namespace apps

}  // namespace xam
}  // namespace rex
