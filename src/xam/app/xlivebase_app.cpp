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
#include <rex/platform/thread.h>
#include <rex/xam/title_lifecycle.h>
#include "xam/stub_log.h"

#include <cstring>

namespace rex {
namespace xam {

using namespace rex::kernel;
namespace apps {
using namespace rex::kernel;

namespace {

// XLIVEBASE splits its message space in two families:
//
//   0x00058xxx  fixed, one message per API (GetLogonId, GetNatType, ...)
//   0x0005_0sss  generic "XOnline task" RPC; `sss` is a service id looked up at
//                runtime from the title's task table, so the message number is
//                data driven and every one of them shares a single ABI.
//
// The task family payload is always 40 bytes:
//
//   +0x00  be<u32>  pointer to the guest task context (args already marshalled
//                   into it; the context holds the serialized blob at +0x1C)
//   +0x04  be<u32>  reserved, always 0
//   +0x08  u8[32]   zero filled by the caller
//
// Result data never travels in this buffer -- the caller passes its own output
// buffer down through the task context and, in every observed call site, zeroes
// it before dispatching. That means completing the task successfully without
// touching anything yields a well formed "zero results" answer, which is the
// path titles take when a query legitimately finds nothing. Returning a failure
// HRESULT instead drives them into their error path, so success-with-no-data is
// both the safer and the more accurate emulation for an offline runtime.
struct X_ONLINE_TASK_MESSAGE {
  be_u32 task_ptr;
  be_u32 reserved;
  u8 out[32];
};
static_assert(sizeof(X_ONLINE_TASK_MESSAGE) == 40, "XOnline task message must be 40 bytes");

// True for the 0x0005_0sss task family, false for the fixed 0x00058xxx APIs.
// Service ids are single byte values, so bit 15 cleanly separates the two.
constexpr bool IsOnlineTaskMessage(u32 message) {
  return (message & 0xFFFF0000) == 0x00050000 && !(message & 0x8000);
}

// Handles the 0x0005xxxx "XOnline task" message family (see above). Completes the task as a
// successful no-op / empty result.
kernel::X_HRESULT HandleOnlineTask(const rex::memory::GuestMembase& membase, u32 message,
                                   u32 buffer_ptr, u32 buffer_length) {
  const u32 service_id = message & 0xFFFF;

  if (buffer_length && buffer_length != sizeof(X_ONLINE_TASK_MESSAGE)) {
    REXXAM_WARN("XOnlineTask(service {:04X}): unexpected buffer length {} (expected {})",
                service_id, buffer_length, sizeof(X_ONLINE_TASK_MESSAGE));
  }

  u32 task_ptr = 0;
  if (buffer_ptr && buffer_length >= 8) {
    task_ptr = memory::load_and_swap<u32>(membase.TranslateVirtual(buffer_ptr));
  }

  // [invite-probe] XInviteSend is service 0x0002. Log at INFO so the invite send
  // is visible; still completes with no results for now.
  REXXAM_INFO("[invite-probe] XOnlineTask service={:04X} task={:08X} -> no-op (no results)",
              service_id, task_ptr);
  return kernel::X_E_SUCCESS;
}

}  // namespace

XLiveBaseApp::XLiveBaseApp(const XAMKernelInterfaces& title) : rex::xam::XAMApplet(title, 0xFC) {}

kernel::X_HRESULT XLiveBaseApp::DispatchMessageSync(u32 message, u32 buffer_ptr,
                                                    u32 buffer_length) {
  auto buffer = membase_.TranslateVirtual(buffer_ptr);

  // [invite-probe] Log XLIVEBASE messages so an in-cruise race invite shows its
  // path. Skip the per-frame polls that otherwise flood the log:
  //   0x0005800E session module poll, 0x00058009 IsOnline, 0x00058004 GetLogonId.
  // Result so far: invites do NOT go through XLIVEBASE (no 0x0005_0002 XInviteSend
  // seen), so the challenge is a game/session mechanism, not this path.
  if (message != 0x0005800E && message != 0x00058009 && message != 0x00058004) {
    REXXAM_INFO("[invite-probe] XLIVEBASE msg={:08X} buf={:08X} len={}", message, buffer_ptr,
                buffer_length);
  }

  switch (message) {
    case 0x00058004: {
      REX_ASSERT(!buffer_length || buffer_length == 4);
      REXXAM_DEBUG("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      memory::store<be_u32>(buffer + 0, 1);
      return kernel::X_E_SUCCESS;
    }
    case 0x00058006: {
      REX_ASSERT(!buffer_length || buffer_length == 4);
      REXXAM_DEBUG("XLiveBaseGetNatType({:08X})", buffer_ptr);
      // XONLINE_NAT_OPEN
      memory::store<be_u32>(buffer + 0, 1);
      return kernel::X_E_SUCCESS;
    }
    case 0x00058007: {
      // The buffer holds dwServiceId and a pServiceInfo pointer to XONLINE_SERVICE_INFO.
      REXXAM_DEBUG("CXLiveLogon::GetServiceInfo({:08X}, {:08X})", buffer_ptr, buffer_length);
      // XONLINE_E_LOGON_NO_NETWORK_CONNECTION
      return 0x80151802;
    }
    case 0x00058009: {
      // CXLiveLogon::IsOnline / GetLogonUsers: the game checks Xbox Live status.
      REXXAM_DEBUG("CXLiveLogon::IsOnline({:08X}, {:08X})", buffer_ptr, buffer_length);
      if (buffer_length >= 4) {
        std::memset(buffer, 0, buffer_length);
      }
      return kernel::X_E_SUCCESS;
    }
    case 0x00058020: {
      REX_XAM_STUB("CXLiveFriends::Enumerate({:08X}, {:08X})", buffer_ptr, buffer_length);
      return kernel::X_E_FAIL;
    }
    case 0x00058023: {
      REX_XAM_STUB("CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X})", buffer_ptr,
                   buffer_length);
      return kernel::X_E_FAIL;
    }
    case 0x00058035: {
      REXXAM_DEBUG("XLiveBase::Unk58035({:08X}, {:08X})", buffer_ptr, buffer_length);
      return kernel::X_E_SUCCESS;
    }
    case 0x0005800E: {
      // Session module call (guest sub_8274B3F8, XMsgInProcessCall id 360462).
      // Both args are guest pointers ~0x70 apart (a request struct and a result
      // struct), not a ptr/length pair. The guest reads its result from the
      // second struct, which it lays out before calling; the shipped XAM path
      // fills a status word there. Leaving it as the guest initialized it and
      // returning success is the benign offline answer -- same treatment as the
      // other fixed 0x58xxx session calls above. Revisit if a title reads a
      // field we must populate.
      REXXAM_DEBUG("XLiveBase::SessionUnk5800E({:08X}, {:08X})", buffer_ptr, buffer_length);
      return kernel::X_E_SUCCESS;
    }
    case 0x00058046: {
      REX_XAM_STUB("XLiveBaseUnk58046({:08X}, {:08X})", buffer_ptr, buffer_length);
      return kernel::X_E_SUCCESS;
    }
    case 0x00058037: {
      REXXAM_DEBUG("XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return kernel::X_E_SUCCESS;
    }
    default:
      break;
  }

  // 0x0005_0sss task family. Known service ids seen in shipped titles:
  //   0x0002  XInviteSend            (task 0x0583)
  //   0x0009  XStorageDownloadToMemory (task 0x0E11)
  //   0x000A  XStorageEnumerate      (task 0x0E14)
  //   0x000C  task 0x0801
  // The list is title specific because the service id comes from the title's
  // own task table, so handle the whole family generically rather than
  // enumerating ids.
  if (IsOnlineTaskMessage(message)) {
    return HandleOnlineTask(membase_, message, buffer_ptr, buffer_length);
  }

  REXXAM_FAIL(kernel::X_E_FAIL,
              "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
              "arg2={:08X}",
              GetAppID(), message, buffer_ptr, buffer_length);
}

}  // namespace apps

}  // namespace xam
}  // namespace rex
