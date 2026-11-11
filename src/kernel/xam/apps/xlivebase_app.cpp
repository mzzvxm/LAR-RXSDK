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

#include <rex/kernel/xam/apps/xlivebase_app.h>
#include <rex/logging.h>
#include <rex/thread.h>

#include <cstring>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state) : App(kernel_state, 0xFC) {}

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
  rex::be<uint32_t> task_ptr;
  rex::be<uint32_t> reserved;
  uint8_t out[32];
};
static_assert(sizeof(X_ONLINE_TASK_MESSAGE) == 40,
              "XOnline task message must be 40 bytes");

// True for the 0x0005_0sss task family, false for the fixed 0x00058xxx APIs.
// Service ids are single byte values, so bit 15 cleanly separates the two.
static constexpr bool IsOnlineTaskMessage(uint32_t message) {
  return (message & 0xFFFF0000) == 0x00050000 && !(message & 0x8000);
}

X_HRESULT XLiveBaseApp::HandleOnlineTask(uint32_t message, uint32_t buffer_ptr,
                                         uint32_t buffer_length) {
  const uint32_t service_id = message & 0xFFFF;

  if (buffer_length && buffer_length != sizeof(X_ONLINE_TASK_MESSAGE)) {
    REXKRNL_WARN("XOnlineTask(service {:04X}): unexpected buffer length {} (expected {})",
                 service_id, buffer_length, sizeof(X_ONLINE_TASK_MESSAGE));
  }

  uint32_t task_ptr = 0;
  if (buffer_ptr && buffer_length >= 8) {
    task_ptr = memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(buffer_ptr));
  }

  // [invite-probe] XInviteSend is service 0x0002. Log at INFO so the invite send
  // is visible; still completes with no results for now.
  REXKRNL_INFO("[invite-probe] XOnlineTask service={:04X} task={:08X} -> no-op (no results)",
               service_id, task_ptr);
  return X_E_SUCCESS;
}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);

  // [invite-probe] Log XLIVEBASE messages so an in-cruise race invite shows its
  // path. Skip the per-frame polls that otherwise flood the log:
  //   0x0005800E session module poll, 0x00058009 IsOnline, 0x00058004 GetLogonId.
  // Result so far: invites do NOT go through XLIVEBASE (no 0x0005_0002 XInviteSend
  // seen), so the challenge is a game/session mechanism, not this path.
  if (message != 0x0005800E && message != 0x00058009 && message != 0x00058004) {
    REXKRNL_INFO("[invite-probe] XLIVEBASE msg={:08X} buf={:08X} len={}", message, buffer_ptr,
                 buffer_length);
  }

  switch (message) {
    case 0x00058004: {
      // Called on startup, seems to just return a bool in the buffer.
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetNatType({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // XONLINE_NAT_OPEN
      return X_E_SUCCESS;
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      REXKRNL_DEBUG("CXLiveLogon::GetServiceInfo({:08X}, {:08X})", buffer_ptr, buffer_length);
      return 0x80151802;  // ERROR_CONNECTION_INVALID
    }
    case 0x00058020: {
      // 0x00058004 is called right before this.
      // We should create a XamEnumerate-able empty list here, but I'm not
      // sure of the format.
      // buffer_length seems to be the same ptr sent to 0x00058004.
      REXKRNL_DEBUG("CXLiveFriends::Enumerate({:08X}, {:08X}) unimplemented", buffer_ptr,
                    buffer_length);
      return X_E_FAIL;
    }
    case 0x00058023: {
      REXKRNL_DEBUG(
          "CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X}) "
          "unimplemented",
          buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      REXKRNL_DEBUG("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058009: {
      // CXLiveLogon::IsOnline / GetLogonUsers — game checks Xbox Live status.
      REXKRNL_DEBUG("CXLiveLogon::IsOnline({:08X}, {:08X})", buffer_ptr, buffer_length);
      if (buffer_length >= 4) {
        std::memset(buffer, 0, buffer_length);
      }
      return X_E_SUCCESS;
    }
    case 0x00058035: {
      REXKRNL_DEBUG("XLiveBase::Unk58035({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
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
      REXKRNL_DEBUG("XLiveBase::SessionUnk5800E({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058037: {
      REXKRNL_DEBUG("XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
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
    return HandleOnlineTask(message, buffer_ptr, buffer_length);
  }

  REXKRNL_ERROR(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
