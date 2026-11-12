/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#if REX_PLATFORM_MAC
#include <sys/select.h>
#endif

#include <rex/chrono/clock.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#include "mcla_online.h"

#if REX_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include <winsock2.h>                    // NOLINT(build/include_order)
#include <ws2tcpip.h>                    // socklen_t
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>

#include <rex/net/posix_compat.h>
#endif

REXCVAR_DECLARE(bool, mcla_online_enable);
REXCVAR_DECLARE(int32_t, mcla_online_connect_timeout_ms);

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

// Guest socket descriptors. On real hardware NetDll_socket hands the title a
// small, positive, fd-like value, and titles do signed validity checks on it
// (e.g. Midnight Club's RAGE LAN listener gates getsockname on `handle >= 0`).
// Our kernel object handles are kHandleBase-based, and kHandleBase (0xF8000000)
// has bit31 set, so those handles read as negative int32 and fail the check.
// Present sockets to the guest with bit31 cleared, and restore it before every
// object-table lookup. The transform is lossless because kHandleBase keeps
// bit31 set (asserted below); nothing here relies on any other handle bits.
static_assert((XObject::kHandleBase & 0x80000000u) != 0u,
              "socket fd remap assumes kHandleBase keeps bit31 set");
inline u32 SocketHandleToGuest(X_HANDLE handle) {
  return static_cast<u32>(handle) & 0x7FFFFFFFu;
}
inline X_HANDLE GuestToSocketHandle(u32 fd) {
  return static_cast<X_HANDLE>(fd | 0x80000000u);
}

// Hands the profile gamertag to the online client so every register/heartbeat
// carries it. Only safe from a guest thread (touches the kernel state); the
// client caches the copy its heartbeat thread sends.
static void PushGamertag(OnlineClient& oc) {
  auto* profile = REX_KERNEL_STATE()->user_profile();
  if (profile) {
    oc.SetGamertag(profile->name().c_str());
    oc.SetXuid(profile->xuid());
  }
}

// https://github.com/G91/TitanOffLine/blob/1e692d9bb9dfac386d08045ccdadf4ae3227bb5e/xkelib/xam/xamNet.h
enum {
  XNCALLER_INVALID = 0x0,
  XNCALLER_TITLE = 0x1,
  XNCALLER_SYSAPP = 0x2,
  XNCALLER_XBDM = 0x3,
  XNCALLER_TEST = 0x4,
  NUM_XNCALLER_TYPES = 0x4,
};

// https://github.com/pmrowla/hl2sdk-csgo/blob/master/common/xbox/xboxstubs.h
typedef struct {
  // FYI: IN_ADDR should be in network-byte order.
  in_addr ina;                    // IP address (zero if not static/DHCP)
  in_addr inaOnline;              // Online IP address (zero if not online)
  rex::be<uint16_t> wPortOnline;  // Online port
  uint8_t abEnet[6];              // Ethernet MAC address
  uint8_t abOnline[20];           // Online identification
} XNADDR;

typedef struct {
  rex::be<int32_t> status;
  rex::be<uint32_t> cina;
  in_addr aina[8];
} XNDNS;

typedef struct {
  uint8_t flags;
  uint8_t reserved;
  rex::be<uint16_t> probes_xmit;
  rex::be<uint16_t> probes_recv;
  rex::be<uint16_t> data_len;
  rex::be<uint32_t> data_ptr;
  rex::be<uint16_t> rtt_min_in_msecs;
  rex::be<uint16_t> rtt_med_in_msecs;
  rex::be<uint32_t> up_bits_per_sec;
  rex::be<uint32_t> down_bits_per_sec;
} XNQOSINFO;

typedef struct {
  rex::be<uint32_t> count;
  rex::be<uint32_t> count_pending;
  XNQOSINFO info[1];
} XNQOS;

struct Xsockaddr_t {
  rex::be<uint16_t> sa_family;
  char sa_data[14];
};

struct X_WSADATA {
  rex::be<uint16_t> version;
  rex::be<uint16_t> version_high;
  char description[256 + 1];
  char system_status[128 + 1];
  rex::be<uint16_t> max_sockets;
  rex::be<uint16_t> max_udpdg;
  rex::be<uint32_t> vendor_info_ptr;
};

struct XWSABUF {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf_ptr;
};

struct XWSAOVERLAPPED {
  rex::be<uint32_t> internal;
  rex::be<uint32_t> internal_high;
  union {
    struct {
      rex::be<uint32_t> low;
      rex::be<uint32_t> high;
    } offset;  // must be named to avoid GCC error
    rex::be<uint32_t> pointer;
  };
  rex::be<uint32_t> event_handle;
};

inline void CompleteWSAOverlapped(ppc_ptr_t<XWSAOVERLAPPED> overlapped, uint32_t status, uint32_t bytes_transferred) {
  if (overlapped) {
    overlapped->internal = status;
    overlapped->internal_high = bytes_transferred;
    uint32_t ev_handle = overlapped->event_handle;
    if (ev_handle) {
      auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(ev_handle);
      if (ev) {
        ev->Set(0, false);
      }
    }
  }
}

void LoadSockaddr(const uint8_t* ptr, sockaddr* out_addr) {
  out_addr->sa_family = memory::load_and_swap<uint16_t>(ptr + 0);
  switch (out_addr->sa_family) {
    case AF_INET: {
      auto in_addr = reinterpret_cast<sockaddr_in*>(out_addr);
      in_addr->sin_port = memory::load_and_swap<uint16_t>(ptr + 2);
      // Maybe? Depends on type.
      in_addr->sin_addr.s_addr = *(uint32_t*)(ptr + 4);
      break;
    }
    default:
      assert_unhandled_case(out_addr->sa_family);
      break;
  }
}

void StoreSockaddr(const sockaddr& addr, uint8_t* ptr) {
  switch (addr.sa_family) {
    case AF_UNSPEC:
      std::memset(ptr, 0, sizeof(addr));
      break;
    case AF_INET: {
      auto& in_addr = reinterpret_cast<const sockaddr_in&>(addr);
      memory::store_and_swap<uint16_t>(ptr + 0, in_addr.sin_family);
      memory::store_and_swap<uint16_t>(ptr + 2, in_addr.sin_port);
      // Maybe? Depends on type.
      memory::store_and_swap<uint32_t>(ptr + 4, in_addr.sin_addr.s_addr);
      break;
    }
    default:
      assert_unhandled_case(addr.sa_family);
      break;
  }
}

// https://github.com/joolswills/mameox/blob/master/MAMEoX/Sources/xbox_Network.cpp#L136
struct XNetStartupParams {
  uint8_t cfgSizeOfStruct;
  uint8_t cfgFlags;
  uint8_t cfgSockMaxDgramSockets;
  uint8_t cfgSockMaxStreamSockets;
  uint8_t cfgSockDefaultRecvBufsizeInK;
  uint8_t cfgSockDefaultSendBufsizeInK;
  uint8_t cfgKeyRegMax;
  uint8_t cfgSecRegMax;
  uint8_t cfgQosDataLimitDiv4;
  uint8_t cfgQosProbeTimeoutInSeconds;
  uint8_t cfgQosProbeRetries;
  uint8_t cfgQosSrvMaxSimultaneousResponses;
  uint8_t cfgQosPairWaitTimeInSeconds;
};

XNetStartupParams xnet_startup_params = {};

u32 NetDll_XNetStartup_entry(u32 caller, ppc_ptr_t<XNetStartupParams> params) {
  if (params) {
    assert_true(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }

  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");

  /*
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return 0;
}

u32 NetDll_XNetCleanup_entry(u32 caller, mapped_void params) {
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  // auto xnet = xam->xnet();
  // xam->set_xnet(nullptr);

  // TODO: Shut down and delete.
  // delete xnet;

  return 0;
}

u32 NetDll_XNetGetOpt_entry(u32 one, u32 option_id, mapped_void buffer_ptr,
                            mapped_u32 buffer_size) {
  assert_true(one == 1);
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        return 0x2738;  // WSAEMSGSIZE
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      return 0;
    default:
      REXKRNL_ERROR("NetDll_XNetGetOpt: option {} unimplemented", option_id);
      return 0x2726;  // WSAEINVAL
  }
}

u32 NetDll_XNetRandom_entry(u32 caller, mapped_void buffer_ptr, u32 length) {
  // For now, constant values.
  // This makes replicating things easier.
  std::memset(buffer_ptr, 0xBB, length);

  return 0;
}

u32 NetDll_WSAStartup_entry(u32 caller, u16 version, ppc_ptr_t<X_WSADATA> data_ptr) {
// TODO(benvanik): abstraction layer needed.
#if REX_PLATFORM_WIN32
  WSADATA wsaData;
  ZeroMemory(&wsaData, sizeof(WSADATA));
  int ret = WSAStartup(version, &wsaData);

  auto data_out = REX_KERNEL_MEMORY()->TranslateVirtual(data_ptr.guest_address());

  if (data_ptr) {
    data_ptr->version = wsaData.wVersion;
    data_ptr->version_high = wsaData.wHighVersion;
    std::memcpy(&data_ptr->description, wsaData.szDescription, 0x100);
    std::memcpy(&data_ptr->system_status, wsaData.szSystemStatus, 0x80);
    data_ptr->max_sockets = wsaData.iMaxSockets;
    data_ptr->max_udpdg = wsaData.iMaxUdpDg;

    // Some games (5841099F) want this value round-tripped - they'll compare if
    // it changes and bugcheck if it does.
    uint32_t vendor_ptr = memory::load_and_swap<uint32_t>(data_out + 0x190);
    memory::store_and_swap<uint32_t>(data_out + 0x190, vendor_ptr);
  }
#else
  int ret = 0;
  if (data_ptr) {
    // Guess these values!
    data_ptr->version = version;
    data_ptr->description[0] = '\0';
    data_ptr->system_status[0] = '\0';
    data_ptr->max_sockets = 100;
    data_ptr->max_udpdg = 1024;
  }
#endif

  // DEBUG
  /*
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return ret;
}

u32 NetDll_WSACleanup_entry(u32 caller) {
  // This does nothing. Xenia needs WSA running.
  return 0;
}

u32 NetDll_WSAGetLastError_entry() {
  return XThread::GetLastError();
}

u32 NetDll_WSARecvFrom_entry(u32 caller, u32 socket, ppc_ptr_t<XWSABUF> buffers_ptr,
                             u32 buffer_count, mapped_u32 num_bytes_recv, mapped_u32 flags_ptr,
                             ppc_ptr_t<XSOCKADDR_IN> from_addr,
                             ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                             mapped_void completion_routine_ptr) {
  if (overlapped_ptr) {
    // auto evt = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(
    //    overlapped_ptr->event_handle);

    // if (evt) {
    //  //evt->Set(0, false);
    //}
  }

  if (OnlineClient::Enabled() && buffer_count > 0) {
    auto sock = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(socket));
    if (sock) {
      if (!sock->HasQueuedPackets()) {
        XThread::SetLastError(0x2733);  // WSAEWOULDBLOCK
        return -1;
      }
      XWSABUF b0 = buffers_ptr[0];
      uint8_t* dst = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(b0.buf_ptr);
      N_XSOCKADDR_IN from{};
      uint32_t fromlen = sizeof(from);
      int ret = sock->RecvFrom(dst, b0.len, 0, &from, &fromlen);
      if (ret >= 0) {
        if (num_bytes_recv) *num_bytes_recv = ret;
        if (from_addr) {
          from_addr->sin_family = 2;
          from_addr->sin_addr = from.sin_addr;
          from_addr->sin_port = from.sin_port;
        }
        uint32_t src_ip = from.sin_addr;
        uint16_t src_port = from.sin_port;
        REXLOG_INFO("[mcla-online] WSARecvFrom: received {} bytes from {}.{}.{}.{}:{}",
                    ret, (src_ip >> 24) & 0xFF, (src_ip >> 16) & 0xFF,
                    (src_ip >> 8) & 0xFF, src_ip & 0xFF, src_port);
        return 0;
      }
      XThread::SetLastError(0x2733);  // WSAEWOULDBLOCK
      return -1;
    }
  }

  // we're not going to be receiving packets any time soon
  // return error so we don't wait on that - Cancerous
  return -1;
}

// If the socket is a VDP socket, buffer 0 is the game data length, and buffer 1
// is the unencrypted game data.
u32 NetDll_WSASendTo_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers,
                           u32 num_buffers, mapped_u32 num_bytes_sent, u32 flags,
                           ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len,
                           ppc_ptr_t<XWSAOVERLAPPED> overlapped, mapped_void completion_routine) {
  assert(!completion_routine);

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(socket_handle));
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // Our sockets implementation doesn't support multiple buffers, so we need
  // to combine the buffers the game has given us!
  std::vector<uint8_t> combined_buffer_mem;
  uint32_t combined_buffer_size = 0;
  uint32_t combined_buffer_offset = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    combined_buffer_size += buffers[i].len;
    combined_buffer_mem.resize(combined_buffer_size);
    uint8_t* combined_buffer = combined_buffer_mem.data();

    std::memcpy(combined_buffer + combined_buffer_offset,
                REX_KERNEL_MEMORY()->TranslateVirtual(buffers[i].buf_ptr), buffers[i].len);
    combined_buffer_offset += buffers[i].len;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  if (OnlineClient::Enabled()) {
    uint32_t dest_ip = native_to.sin_addr;  // host order
    uint16_t dest_port = native_to.sin_port;
    REXLOG_INFO("[mcla-online] WSASendTo: sending {} bytes to {}.{}.{}.{}:{}",
                combined_buffer_size, (dest_ip >> 24) & 0xFF, (dest_ip >> 16) & 0xFF,
                (dest_ip >> 8) & 0xFF, dest_ip & 0xFF, dest_port);
    if (dest_ip == 0xFFFFFFFFu || (dest_ip & 0x000000FFu) == 0x000000FFu) {
      // LAN broadcast discovery -> relay to all peers.
      OnlineClient::Get().SendRelay(0, dest_port, combined_buffer_mem.data(),
                                    static_cast<uint16_t>(combined_buffer_size));
      if (num_bytes_sent) *num_bytes_sent = combined_buffer_size;
      return 0;
    }
    if ((dest_ip & 0xFF000000u) == 0x0A000000u) {
      // Unicast to a peer's virtual ip -> relay to that peer.
      OnlineClient::Get().SendRelay(dest_ip, dest_port, combined_buffer_mem.data(),
                                    static_cast<uint16_t>(combined_buffer_size));
      if (num_bytes_sent) *num_bytes_sent = combined_buffer_size;
      return 0;
    }
  }
  socket->SendTo(combined_buffer_mem.data(), combined_buffer_size, flags, &native_to, to_len);

  if (num_bytes_sent) *num_bytes_sent = combined_buffer_size;

  return 0;
}

u32 NetDll_WSAWaitForMultipleEvents_entry(u32 num_events, mapped_u32 events, u32 wait_all,
                                          u32 timeout, u32 alertable) {
  if (num_events > 64) {
    XThread::SetLastError(87);  // ERROR_INVALID_PARAMETER
    return ~0u;
  }

  uint64_t timeout_wait = (uint64_t)timeout;

  X_STATUS result = 0;
  do {
    result = xboxkrnl::xeNtWaitForMultipleObjectsEx(num_events, events, wait_all, 1, alertable,
                                                    timeout != -1 ? &timeout_wait : nullptr);
  } while (result == X_STATUS_ALERTED);

  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return ~0u;
  }
  return 0;
}

u32 NetDll_WSACreateEvent_entry() {
  auto ev = object_ref<XEvent>(new XEvent(REX_KERNEL_STATE()));
  ev->Initialize(true, false);
  return ev->handle();
}

u32 NetDll_WSACloseEvent_entry(u32 event_handle) {
  X_STATUS result = REX_KERNEL_OBJECTS()->ReleaseHandle(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSAEventSelect_entry(u32 caller, u32 socket_handle, u32 event_handle, u32 network_events) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(socket_handle));
  if (!socket) {
    XThread::SetLastError(0x2736);  // WSAENOTSOCK
    return -1;
  }
  socket->set_event_handle(event_handle);
  return 0;
}

u32 NetDll_WSAResetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSASetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

struct XnAddrStatus {
  // Address acquisition is not yet complete
  static const uint32_t XNET_GET_XNADDR_PENDING = 0x00000000;
  // XNet is uninitialized or no debugger found
  static const uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
  // Host has ethernet address (no IP address)
  static const uint32_t XNET_GET_XNADDR_ETHERNET = 0x00000002;
  // Host has statically assigned IP address
  static const uint32_t XNET_GET_XNADDR_STATIC = 0x00000004;
  // Host has DHCP assigned IP address
  static const uint32_t XNET_GET_XNADDR_DHCP = 0x00000008;
  // Host has PPPoE assigned IP address
  static const uint32_t XNET_GET_XNADDR_PPPOE = 0x00000010;
  // Host has one or more gateways configured
  static const uint32_t XNET_GET_XNADDR_GATEWAY = 0x00000020;
  // Host has one or more DNS servers configured
  static const uint32_t XNET_GET_XNADDR_DNS = 0x00000040;
  // Host is currently connected to online service
  static const uint32_t XNET_GET_XNADDR_ONLINE = 0x00000080;
  // Network configuration requires troubleshooting
  static const uint32_t XNET_GET_XNADDR_TROUBLESHOOT = 0x00008000;
};

u32 NetDll_XNetGetTitleXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  // The whole 36-byte XNADDR is an out-parameter and titles compare XNADDRs
  // byte-exact over all 36 bytes (MCLA: sub_82501A88, which is how the RAGE
  // session decides which peer is the broadcast-group host). Only the first 16
  // bytes are filled below, so abOnline[20] would otherwise keep whatever the
  // caller's buffer happened to hold. Every other XNADDR producer here
  // (XNetInAddrToXnAddr, XSESSION_INFO in xgi_app) already zeroes abOnline;
  // match them so the comparisons are well-defined for callers that do not
  // pre-clear the struct.
  addr_ptr.Zero();

  uint32_t my_ip = INADDR_LOOPBACK;
  uint16_t game_port = 1000;
  if (OnlineClient::Enabled()) {
    auto& oc = OnlineClient::Get();
    oc.EnsureStarted();

    if (!oc.connection_checked()) {
      // Wait for the server's first ack. Over the internet / a VPN the round
      // trip and packet loss easily exceed a few hundred ms, so budget
      // generously (configurable) and resend the register periodically so a
      // dropped first datagram doesn't falsely report the server offline.
      int cfg_budget = REXCVAR_GET(mcla_online_connect_timeout_ms);
      uint32_t budget_ms = cfg_budget > 0 ? static_cast<uint32_t>(cfg_budget) : 3000u;
      const uint32_t resend_every_ms = 400;
      uint32_t since_resend = resend_every_ms;  // probe immediately on entry
      while (GetTickCount() - oc.startup_time_ms() < budget_ms && !oc.connection_checked()) {
        if (since_resend >= resend_every_ms) {
          oc.Reprobe();
          since_resend = 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        since_resend += 10;
      }
      if (!oc.connection_checked()) {
        oc.set_connection_checked(true);
      }
    }

    if (oc.server_online()) {
      uint32_t vip = oc.virtual_ip();
      if (vip != 0) {
        my_ip = vip;
      }
      uint16_t gp = oc.game_port();
      if (gp != 0) {
        game_port = gp;
      }
    } else {
      REXLOG_ERROR("[mcla-online] Server is offline (no response), disabling mcla_online_enable.");
      REXCVAR_SET(mcla_online_enable, false);
    }
  }
  addr_ptr->ina.s_addr = htonl(my_ip);
  addr_ptr->inaOnline.s_addr = htonl(my_ip);
  addr_ptr->wPortOnline = game_port;

  // TODO(gibbed): A proper mac address.
  // RakNet's 360 version appears to depend on abEnet to create "random" 64-bit
  // numbers. A zero value will cause RakPeer::Startup to fail. This causes
  // 58411436 to crash on startup.
  // The 360-specific code is scrubbed from the RakNet repo, but there's still
  // traces of what it's doing which match the game code.
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L382
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4527
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4467
  // "Mac address is a poor solution because you can't have multiple connections
  // from the same system"
  std::memset(addr_ptr->abEnet, 0xCC, 6);
  // Make the MAC differ per instance (derived from the virtual ip).
  addr_ptr->abEnet[4] = (my_ip >> 8) & 0xFF;
  addr_ptr->abEnet[5] = my_ip & 0xFF;

  u32 flags = OnlineClient::Enabled()
             ? (XnAddrStatus::XNET_GET_XNADDR_STATIC | XnAddrStatus::XNET_GET_XNADDR_GATEWAY |
                XnAddrStatus::XNET_GET_XNADDR_DNS | XnAddrStatus::XNET_GET_XNADDR_ONLINE)
             : XnAddrStatus::XNET_GET_XNADDR_STATIC;
  return flags;
}

u32 NetDll_XNetGetDebugXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  addr_ptr.Zero();

  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}

u32 NetDll_XNetXnAddrToMachineId_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr, mapped_u32 id_ptr) {
  // Tell the caller we're not signed in to live (non-zero ret)
  return 1;
}

void NetDll_XNetInAddrToString_entry(u32 caller, u32 in_addr, mapped_string string_out,
                                     u32 string_size) {
  rex::string::copy_truncating(string_out, "666.666.666.666", string_size);
}

// Remembers the real wPortOnline seen for each remote virtual ip. XNADDRs
// arrive here fully populated (from a search result or the session info), and
// XNetInAddrToXnAddr later has to rebuild the same 36 bytes from just the ip.
// The game compares those 36 bytes for equality (sub_82501A88, used by
// sub_82297B68 to record the group owner at sessionRec+0x2E04), so filling in
// our own local port there produced an XNADDR that never matched and left a
// guest unable to resolve the owner - and therefore unable to start its network
// clock, which silently kills every race challenge.
static std::mutex g_peer_port_mutex;
static std::map<uint32_t, uint16_t> g_peer_ports;

static void RememberPeerPort(uint32_t vip_host, uint16_t port) {
  if (vip_host == 0 || port == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_peer_port_mutex);
  g_peer_ports[vip_host] = port;
}

static uint16_t RecallPeerPort(uint32_t vip_host) {
  std::lock_guard<std::mutex> lock(g_peer_port_mutex);
  auto it = g_peer_ports.find(vip_host);
  return it == g_peer_ports.end() ? 0 : it->second;
}

// This converts a XNet address to an IN_ADDR. The IN_ADDR is used for
// subsequent socket calls (like a handle to a XNet address)
u32 NetDll_XNetXnAddrToInAddr_entry(u32 caller, ppc_ptr_t<XNADDR> xn_addr, mapped_void xid,
                                    mapped_void in_addr) {
  if (OnlineClient::Enabled() && xn_addr && in_addr) {
    uint32_t vip_host = ntohl(xn_addr->ina.s_addr);  // ina is network order
    RememberPeerPort(vip_host, xn_addr->wPortOnline);
    auto ptr = REX_KERNEL_MEMORY()->TranslateVirtual<uint32_t*>(in_addr.guest_address());
    if (ptr) {
      *ptr = rex::byte_swap(vip_host); // big-endian
    }
    // Signals the game is committing to connect to a found host (resolving its
    // XNADDR to a sendable address). If this never logs during a join, the game
    // is stuck in matchmaking before the connect step.
    REXKRNL_INFO("[online-diag] XNetXnAddrToInAddr -> vip 10.0.0.{}", vip_host & 0xFF);
    return 0;
  }
  return 1;
}

// Does the reverse of the above.
// FIXME: Arguments may not be correct.
u32 NetDll_XNetInAddrToXnAddr_entry(u32 caller, u32 in_addr, ppc_ptr_t<XNADDR> xn_addr,
                                    mapped_void xid) {
  if (OnlineClient::Enabled() && xn_addr) {
    uint32_t vip_host = in_addr;  // host-order vip (matches XnAddrToInAddr output)
    xn_addr->ina.s_addr = htonl(vip_host);
    xn_addr->inaOnline.s_addr = htonl(vip_host);
    {
      // Prefer the port this peer was actually seen on. Falling back to our own
      // game_port only makes sense when the ip is us.
      uint16_t game_port = RecallPeerPort(vip_host);
      if (game_port == 0) {
        game_port = OnlineClient::Get().game_port();
      }
      if (game_port == 0) game_port = 1000;
      xn_addr->wPortOnline = game_port;
    }
    std::memset(xn_addr->abEnet, 0xCC, 6);
    xn_addr->abEnet[4] = (vip_host >> 8) & 0xFF;
    xn_addr->abEnet[5] = vip_host & 0xFF;
    std::memset(xn_addr->abOnline, 0, 20);
    return 0;
  }
  return 1;
}

u32 NetDll_XNetConnect_entry(u32 caller, u32 in_addr) {
  REXKRNL_INFO("[online-diag] XNetConnect(in_addr={:08X})", in_addr);
  return 0;
}

u32 NetDll_XNetGetConnectStatus_entry(u32 caller, u32 in_addr) {
  u32 status = OnlineClient::Enabled() ? 2u : 0u;
  return status;
}

u32 XNetLogonGetTitleID_entry() {
  auto module = REX_KERNEL_STATE()->GetExecutableModule();
  u32 tid = module ? module->title_id() : 0;
  return tid;
}

struct XNKID_t {
  uint8_t ab[8];
};

struct XNKEY_t {
  uint8_t ab[16];
};

static uint8_t MclaKeyByte() {
  static uint32_t seed = 0x9E3779B9u;
  seed = seed * 1664525u + 1013904223u;
  return static_cast<uint8_t>(seed >> 24);
}

u32 NetDll_XNetCreateKey_entry(u32 caller, ppc_ptr_t<XNKID_t> pxnkid, ppc_ptr_t<XNKEY_t> pxnkey) {
  if (pxnkid) {
    for (int i = 0; i < 8; ++i) {
      pxnkid->ab[i] = MclaKeyByte();
    }
    pxnkid->ab[0] |= 0x01;  // ensure non-zero
  }
  if (pxnkey) {
    for (int i = 0; i < 16; ++i) {
      pxnkey->ab[i] = MclaKeyByte();
    }
  }
  return 0;
}

u32 NetDll_XNetRegisterKey_entry(u32 caller, ppc_ptr_t<XNKID_t> pxnkid, ppc_ptr_t<XNKEY_t> pxnkey) {
  // No real registration needed for plaintext localhost play.
  return 0;
}

u32 NetDll_XNetUnregisterKey_entry(u32 caller, ppc_ptr_t<XNKID_t> pxnkid) {
  return 0;
}

// https://www.google.com/patents/WO2008112448A1?cl=en
// Reserves a port for use by system link
u32 NetDll_XNetSetSystemLinkPort_entry(u32 caller, u32 port) {
  return 1;
}

// https://github.com/ILOVEPIE/Cxbx-Reloaded/blob/master/src/CxbxKrnl/EmuXOnline.h#L39
struct XEthernetStatus {
  static const uint32_t XNET_ETHERNET_LINK_ACTIVE = 0x01;
  static const uint32_t XNET_ETHERNET_LINK_100MBPS = 0x02;
  static const uint32_t XNET_ETHERNET_LINK_10MBPS = 0x04;
  static const uint32_t XNET_ETHERNET_LINK_FULL_DUPLEX = 0x08;
  static const uint32_t XNET_ETHERNET_LINK_HALF_DUPLEX = 0x10;
};

u32 NetDll_XNetGetEthernetLinkStatus_entry(u32 caller) {
  return XEthernetStatus::XNET_ETHERNET_LINK_ACTIVE;
}

u32 NetDll_XNetDnsLookup_entry(u32 caller, mapped_string host, u32 event_handle, mapped_u32 pdns) {
  // TODO(gibbed): actually implement this
  if (pdns) {
    auto dns_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNDNS));
    auto dns = REX_KERNEL_MEMORY()->TranslateVirtual<XNDNS*>(dns_guest);
    dns->status = 1;  // non-zero = error
    *pdns = dns_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

u32 NetDll_XNetDnsRelease_entry(u32 caller, ppc_ptr_t<XNDNS> dns) {
  if (!dns) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(dns.guest_address());
  return 0;
}

u32 NetDll_XNetQosServiceLookup_entry(u32 caller, u32 flags, u32 event_handle, mapped_u32 pqos) {
  // Set pqos as some games will try accessing it despite non-successful result
  if (pqos) {
    auto qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNQOS));
    auto qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
    qos->count = qos->count_pending = 0;
    *pqos = qos_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

u32 NetDll_XNetQosRelease_entry(u32 caller, ppc_ptr_t<XNQOS> qos) {
  if (!qos) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(qos.guest_address());
  return 0;
}

u32 NetDll_XNetQosListen_entry(u32 caller, mapped_void id, mapped_void data, u32 data_size, u32 r7,
                               u32 flags) {
  if (OnlineClient::Enabled()) {
    auto& oc = OnlineClient::Get();
    if (!oc.advertise_as_host()) {
      // We joined someone else's session (XSessionCreate without
      // XSESSION_CREATE_HOST). Advertising here would put a second, phantom
      // host in the lobby list for the same cruise.
      REXKRNL_INFO("[online-diag] XNetQosListen ignored: joined as guest");
      return 0;
    }
    PushGamertag(oc);
    // Session properties (the event definition) are captured as the game sets
    // them via XGIUserSetPropertyEx/SetContextEx (see xgi_app.cpp). A store read
    // here would run before the game populates them, capturing only defaults.
    // Session name defaults to the host's gamertag so the lobby list shows who
    // is hosting.
    auto* profile = REX_KERNEL_STATE()->user_profile();
    std::string session_name = profile ? profile->name() : std::string();
    oc.RegisterHost(0, 0, 4, 1, session_name.empty() ? "MCLA" : session_name.c_str());
    return 0;
  }
  return X_ERROR_FUNCTION_FAILED;
}

u32 NetDll_XNetQosLookup_entry(u32 caller, u32 cxna, u32 apxna_ptr, u32 apxnkid_ptr,
                                u32 apxnkey_ptr, u32 cina, u32 aina_ptr, u32 adwServiceId_ptr,
                                u32 cProbes, u32 dwBitsPerSec, u32 dwFlags, u32 hEvent,
                                mapped_u32 ppxnqos) {
  uint32_t total = cxna + cina;
  if (total == 0) total = 1;


  // Allocate XNQOS with enough space for all entries.
  uint32_t info_size = static_cast<uint32_t>(sizeof(XNQOSINFO));
  uint32_t qos_size = 8 + total * info_size;  // count(4) + count_pending(4) + N * info
  auto qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(qos_size);
  auto qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
  std::memset(qos, 0, qos_size);
  qos->count = total;
  qos->count_pending = 0;

  for (uint32_t i = 0; i < total; ++i) {
    auto* info = reinterpret_cast<XNQOSINFO*>(
        reinterpret_cast<uint8_t*>(qos) + 8 + i * info_size);
    info->flags = 0x01 | 0x02;  // COMPLETE | TARGET_CONTACTED
    info->probes_xmit = 4;
    info->probes_recv = 4;
    info->data_len = 0;
    info->data_ptr = 0;
    info->rtt_min_in_msecs = 5;
    info->rtt_med_in_msecs = 10;
    info->up_bits_per_sec = 10 * 1024 * 1024;
    info->down_bits_per_sec = 10 * 1024 * 1024;
  }

  if (ppxnqos) *ppxnqos = qos_guest;

  if (hEvent) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(hEvent);
    if (ev) ev->Set(0, false);
  }

  return 0;
}

u32 NetDll_inet_addr_entry(mapped_string addr_ptr) {
  if (!addr_ptr) {
    return -1;
  }

  uint32_t addr = inet_addr(addr_ptr);
  // https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr#return-value
  // Based on console research it seems like x360 uses old version of inet_addr
  // In case of empty string it return 0 instead of -1
  if (addr == -1 && !addr_ptr.value().length()) {
    return 0;
  }

  return rex::byte_swap(addr);
}

u32 NetDll_socket_entry(u32 caller, u32 af, u32 type, u32 protocol) {
  auto socket = object_ref<XSocket>(new XSocket(REX_KERNEL_STATE()));
  X_STATUS result =
      socket->Initialize(XSocket::AddressFamily((uint32_t)af), XSocket::Type((uint32_t)type),
                         XSocket::Protocol((uint32_t)protocol));

  if (XFAILED(result)) {
    socket->ReleaseHandle();

    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return -1;
  }

  return SocketHandleToGuest(socket->handle());
}

u32 NetDll_closesocket_entry(u32 caller, u32 socket_handle) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(socket_handle));
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  if (OnlineClient::Enabled()) {
    OnlineClient::Get().OnCloseSocket(socket.get());
  }

  // TODO: Absolutely delete this object. It is no longer valid after calling
  // closesocket.
  socket->Close();
  socket->ReleaseHandle();
  return 0;
}

i32 NetDll_shutdown_entry(u32 caller, u32 socket_handle, i32 how) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(socket_handle));
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  auto ret = socket->Shutdown(how);
  if (ret == -1) {
#if REX_PLATFORM_WIN32
    uint32_t error_code = WSAGetLastError();
    XThread::SetLastError(error_code);
#else
    XThread::SetLastError(0x0);
#endif
  }
  return ret;
}

u32 NetDll_setsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval_ptr, u32 optlen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(socket_handle));
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->SetOption(level, optname, optval_ptr, optlen);
  return XSUCCEEDED(status) ? 0 : -1;
}

u32 NetDll_ioctlsocket_entry(u32 caller, u32 socket_handle, u32 cmd, mapped_void arg_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(socket_handle));
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->IOControl(cmd, arg_ptr);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  // TODO
  return 0;
}

u32 NetDll_bind_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(socket_handle));
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_name(name);
#if REX_PLATFORM_WIN32
  if (OnlineClient::Enabled()) {
    // Let two local instances "bind" the same system-link port -- real match
    // transport goes through the relay, not this OS socket.
    int yes = 1;
    setsockopt(static_cast<SOCKET>(socket->native_handle()), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<char*>(&yes), sizeof(yes));
  }
#endif
  X_STATUS status = socket->Bind(&native_name, namelen);
  if (XFAILED(status) && !OnlineClient::Enabled()) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  if (OnlineClient::Enabled()) {
    uint16_t port = socket->bound_port();
    auto& oc = OnlineClient::Get();
    PushGamertag(oc);
    oc.OnBind(socket.get(), port);
    return 0;
  }

  return 0;
}

u32 NetDll_connect_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(socket_handle));
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_name(name);
  X_STATUS status = socket->Connect(&native_name, namelen);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_listen_entry(u32 caller, u32 socket_handle, i32 backlog) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(socket_handle));
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->Listen(backlog);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_accept_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> addr_ptr,
                        mapped_u32 addrlen_ptr) {
  if (!addr_ptr) {
    // WSAEFAULT
    XThread::SetLastError(0x271E);
    return -1;
  }

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(socket_handle));
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_addr(addr_ptr);
  int native_len = *addrlen_ptr;
  auto new_socket = socket->Accept(&native_addr, &native_len);
  if (new_socket) {
    addr_ptr->address_family = native_addr.address_family;
    std::memcpy(addr_ptr->sa_data, native_addr.sa_data, *addrlen_ptr - 2);
    *addrlen_ptr = native_len;

    return SocketHandleToGuest(new_socket->handle());
  } else {
    return -1;
  }
}

struct x_fd_set {
  rex::be<uint32_t> fd_count;
  rex::be<uint32_t> fd_array[64];
};

struct host_set {
  uint32_t count;
  object_ref<XSocket> sockets[64];

  void Load(const x_fd_set* guest_set) {
    assert_true(guest_set->fd_count < 64);
    this->count = guest_set->fd_count;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == -1) {
        this->count = i;
        break;
      }
      // Convert from Xenia -> native
      auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(socket_handle));
      if (!socket) {
        this->count = i;
        break;
      }
      this->sockets[i] = socket;
    }
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      guest_set->fd_array[guest_set->fd_count++] = SocketHandleToGuest(socket->handle());
    }
  }

  void Store(fd_set* native_set) {
    FD_ZERO(native_set);
    for (uint32_t i = 0; i < this->count; ++i) {
      FD_SET(this->sockets[i]->native_handle(), native_set);
    }
  }

  void UpdateFrom(fd_set* native_set) {
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      if (FD_ISSET(socket->native_handle(), native_set)) {
        this->sockets[new_count++] = socket;
      }
    }
    this->count = new_count;
  }
};

i32 NetDll_select_entry(i32 caller, i32 nfds, ppc_ptr_t<x_fd_set> readfds,
                        ppc_ptr_t<x_fd_set> writefds, ppc_ptr_t<x_fd_set> exceptfds,
                        mapped_void timeout_ptr) {
  // Save original read fd handles before select() overwrites them, so we can
  // re-check internal packet queues afterward.
  uint32_t orig_read_handles[64] = {};
  uint32_t orig_read_count = 0;
  if (readfds && OnlineClient::Enabled()) {
    orig_read_count = std::min((uint32_t)readfds->fd_count, 64u);
    for (uint32_t i = 0; i < orig_read_count; ++i) {
      orig_read_handles[i] = readfds->fd_array[i];
    }
  }

  host_set host_readfds = {};
  fd_set native_readfds = {};
  if (readfds) {
    host_readfds.Load(readfds);
    host_readfds.Store(&native_readfds);
  }
  host_set host_writefds = {};
  fd_set native_writefds = {};
  if (writefds) {
    host_writefds.Load(writefds);
    host_writefds.Store(&native_writefds);
  }
  host_set host_exceptfds = {};
  fd_set native_exceptfds = {};
  if (exceptfds) {
    host_exceptfds.Load(exceptfds);
    host_exceptfds.Store(&native_exceptfds);
  }
  timeval* timeout_in = nullptr;
  timeval timeout;
  if (timeout_ptr) {
    timeout = {static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[0]),
               static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[1])};
    chrono::Clock::ScaleGuestDurationTimeval(reinterpret_cast<int32_t*>(&timeout.tv_sec),
                                             reinterpret_cast<int32_t*>(&timeout.tv_usec));
    timeout_in = &timeout;
  }
  int ret = select(nfds, readfds ? &native_readfds : nullptr, writefds ? &native_writefds : nullptr,
                   exceptfds ? &native_exceptfds : nullptr, timeout_in);
  if (readfds) {
    host_readfds.UpdateFrom(&native_readfds);
    host_readfds.Store(readfds);
  }
  if (writefds) {
    host_writefds.UpdateFrom(&native_writefds);
    host_writefds.Store(writefds);
  }
  if (exceptfds) {
    host_exceptfds.UpdateFrom(&native_exceptfds);
    host_exceptfds.Store(exceptfds);
  }

  // Online: sockets with internally queued relay packets are invisible to the
  // OS select(). Walk the ORIGINAL read set and re-add any socket that has
  // queued data but wasn't already flagged by the OS.
  if (OnlineClient::Enabled() && readfds && orig_read_count > 0) {
    uint32_t added = 0;
    for (uint32_t i = 0; i < orig_read_count; ++i) {
      auto handle = static_cast<X_HANDLE>(orig_read_handles[i]);
      auto sock = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(handle));
      if (!sock || !sock->HasQueuedPackets()) continue;

      // Check if already present in the post-select readfds
      bool already = false;
      for (uint32_t j = 0; j < readfds->fd_count; ++j) {
        if (readfds->fd_array[j] == handle) { already = true; break; }
      }
      if (!already && readfds->fd_count < 64) {
        readfds->fd_array[readfds->fd_count++] = handle;
      }
      added++;
    }
    if (added > 0 && ret <= 0) {
      ret = added;
    }
  }

  return ret;
}

u32 NetDll_recv_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(socket_handle));
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  return socket->Recv(buf_ptr, buf_len, flags);
}

u32 NetDll_recvfrom_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len,
                          u32 flags, ppc_ptr_t<XSOCKADDR_IN> from_ptr, mapped_u32 fromlen_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(socket_handle));
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_from;
  if (from_ptr) {
    native_from = *from_ptr;
  }
  uint32_t native_fromlen = fromlen_ptr ? fromlen_ptr.value() : 0;
  int ret =
      socket->RecvFrom(buf_ptr, buf_len, flags, &native_from, fromlen_ptr ? &native_fromlen : 0);

  if (from_ptr) {
    from_ptr->sin_family = native_from.sin_family;
    from_ptr->sin_port = native_from.sin_port;
    from_ptr->sin_addr = native_from.sin_addr;
    std::memset(from_ptr->x_sin_zero, 0, sizeof(from_ptr->x_sin_zero));
  }
  if (fromlen_ptr) {
    *fromlen_ptr = native_fromlen;
  }

  if (ret == -1) {
// TODO: Better way of getting the error code
#if REX_PLATFORM_WIN32
    uint32_t error_code = WSAGetLastError();
    XThread::SetLastError(error_code);
#else
    XThread::SetLastError(0x0);
#endif
  }

  return ret;
}

u32 NetDll_send_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(socket_handle));
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  return socket->Send(buf_ptr, buf_len, flags);
}

u32 NetDll_sendto_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags,
                        ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(socket_handle));
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  if (OnlineClient::Enabled()) {
    uint32_t dest_ip = native_to.sin_addr;  // host order
    uint16_t dest_port = native_to.sin_port;
    if (dest_ip == 0xFFFFFFFFu || (dest_ip & 0x000000FFu) == 0x000000FFu) {
      // Broadcast -> relay to all peers.
      OnlineClient::Get().SendRelay(0, dest_port, buf_ptr, static_cast<uint16_t>(buf_len));
      return buf_len;
    }
    if ((dest_ip & 0xFF000000u) == 0x0A000000u) {
      // Unicast to virtual ip -> relay to that peer.
      OnlineClient::Get().SendRelay(dest_ip, dest_port, buf_ptr, static_cast<uint16_t>(buf_len));
      return buf_len;
    }
  }
  return socket->SendTo(buf_ptr, buf_len, flags, &native_to, to_len);
}

u32 NetDll___WSAFDIsSet_entry(u32 socket_handle, ppc_ptr_t<x_fd_set> fd_set) {
  const uint8_t max_fd_count = std::min((uint32_t)fd_set->fd_count, uint32_t(64));
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}

void NetDll_WSASetLastError_entry(u32 error_code) {
  XThread::SetLastError(error_code);
}

u32 NetDll_getpeername_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name,
                             ppc_ptr_t<i32> namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(socket_handle));
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  sockaddr_in nname = {};
  socklen_t nnamelen = sizeof(nname);
  int ret = ::getpeername(static_cast<SOCKET>(socket->native_handle()), (sockaddr*)&nname, &nnamelen);
  if (ret < 0) {
    XThread::SetLastError(WSAGetLastError());
    return -1;
  }

  if (name) {
    name->sin_family = 2; // AF_INET
    name->sin_port = ntohs(nname.sin_port);
    name->sin_addr = ntohl(nname.sin_addr.s_addr);
    std::memset(name->x_sin_zero, 0, sizeof(name->x_sin_zero));
  }
  if (namelen) {
    *namelen = sizeof(XSOCKADDR_IN);
  }
  return 0;
}

u32 NetDll_getsockname_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name,
                             ppc_ptr_t<i32> namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(socket_handle));
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  sockaddr_in nname = {};
  socklen_t nnamelen = sizeof(nname);
  int ret = ::getsockname(static_cast<SOCKET>(socket->native_handle()), (sockaddr*)&nname, &nnamelen);
  if (ret < 0) {
    XThread::SetLastError(WSAGetLastError());
    return -1;
  }

  if (name) {
    name->sin_family = 2; // AF_INET
    name->sin_port = ntohs(nname.sin_port);
    if (OnlineClient::Enabled()) {
      name->sin_addr = OnlineClient::Get().virtual_ip();
    } else {
      name->sin_addr = ntohl(nname.sin_addr.s_addr);
    }
    std::memset(name->x_sin_zero, 0, sizeof(name->x_sin_zero));
  }
  if (namelen) {
    *namelen = sizeof(XSOCKADDR_IN);
  }
  return 0;
}

u32 NetDll_getsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval, ppc_ptr_t<i32> optlen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(GuestToSocketHandle(socket_handle));
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  socklen_t native_optlen = optlen ? *optlen : 0;
  std::vector<uint8_t> temp_buf(native_optlen);
  int ret = ::getsockopt(static_cast<SOCKET>(socket->native_handle()), level, optname,
                         reinterpret_cast<char*>(temp_buf.data()), &native_optlen);
  if (ret < 0) {
    XThread::SetLastError(WSAGetLastError());
    return -1;
  }

  if (optval && native_optlen > 0) {
    std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual(optval.guest_address()), temp_buf.data(), native_optlen);
  }
  if (optlen) {
    *optlen = native_optlen;
  }
  return 0;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__NetDll_XNetStartup, rex::kernel::xam::NetDll_XNetStartup_entry)
REX_EXPORT(__imp__NetDll_XNetCleanup, rex::kernel::xam::NetDll_XNetCleanup_entry)
REX_EXPORT(__imp__NetDll_XNetGetOpt, rex::kernel::xam::NetDll_XNetGetOpt_entry)
REX_EXPORT(__imp__NetDll_XNetRandom, rex::kernel::xam::NetDll_XNetRandom_entry)
REX_EXPORT(__imp__NetDll_WSAStartup, rex::kernel::xam::NetDll_WSAStartup_entry)
REX_EXPORT(__imp__NetDll_WSACleanup, rex::kernel::xam::NetDll_WSACleanup_entry)
REX_EXPORT(__imp__NetDll_WSAGetLastError, rex::kernel::xam::NetDll_WSAGetLastError_entry)
REX_EXPORT(__imp__NetDll_WSARecvFrom, rex::kernel::xam::NetDll_WSARecvFrom_entry)
REX_EXPORT(__imp__NetDll_WSASendTo, rex::kernel::xam::NetDll_WSASendTo_entry)
REX_EXPORT(__imp__NetDll_WSAWaitForMultipleEvents,
           rex::kernel::xam::NetDll_WSAWaitForMultipleEvents_entry)
REX_EXPORT(__imp__NetDll_WSACreateEvent, rex::kernel::xam::NetDll_WSACreateEvent_entry)
REX_EXPORT(__imp__NetDll_WSACloseEvent, rex::kernel::xam::NetDll_WSACloseEvent_entry)
REX_EXPORT(__imp__NetDll_WSAResetEvent, rex::kernel::xam::NetDll_WSAResetEvent_entry)
REX_EXPORT(__imp__NetDll_WSASetEvent, rex::kernel::xam::NetDll_WSASetEvent_entry)
REX_EXPORT(__imp__NetDll_XNetGetTitleXnAddr, rex::kernel::xam::NetDll_XNetGetTitleXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetGetDebugXnAddr, rex::kernel::xam::NetDll_XNetGetDebugXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToMachineId,
           rex::kernel::xam::NetDll_XNetXnAddrToMachineId_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToString, rex::kernel::xam::NetDll_XNetInAddrToString_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToInAddr, rex::kernel::xam::NetDll_XNetXnAddrToInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToXnAddr, rex::kernel::xam::NetDll_XNetInAddrToXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetConnect, rex::kernel::xam::NetDll_XNetConnect_entry)
REX_EXPORT(__imp__NetDll_XNetGetConnectStatus, rex::kernel::xam::NetDll_XNetGetConnectStatus_entry)
REX_EXPORT(__imp__NetDll_XNetCreateKey, rex::kernel::xam::NetDll_XNetCreateKey_entry)
REX_EXPORT(__imp__NetDll_XNetRegisterKey, rex::kernel::xam::NetDll_XNetRegisterKey_entry)
REX_EXPORT(__imp__NetDll_XNetUnregisterKey, rex::kernel::xam::NetDll_XNetUnregisterKey_entry)
REX_EXPORT(__imp__NetDll_XNetSetSystemLinkPort,
           rex::kernel::xam::NetDll_XNetSetSystemLinkPort_entry)
REX_EXPORT(__imp__NetDll_XNetGetEthernetLinkStatus,
           rex::kernel::xam::NetDll_XNetGetEthernetLinkStatus_entry)
REX_EXPORT(__imp__NetDll_XNetDnsLookup, rex::kernel::xam::NetDll_XNetDnsLookup_entry)
REX_EXPORT(__imp__NetDll_XNetDnsRelease, rex::kernel::xam::NetDll_XNetDnsRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosServiceLookup, rex::kernel::xam::NetDll_XNetQosServiceLookup_entry)
REX_EXPORT(__imp__NetDll_XNetQosRelease, rex::kernel::xam::NetDll_XNetQosRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosListen, rex::kernel::xam::NetDll_XNetQosListen_entry)
REX_EXPORT(__imp__XNetLogonGetTitleID, rex::kernel::xam::XNetLogonGetTitleID_entry)
REX_EXPORT(__imp__NetDll_inet_addr, rex::kernel::xam::NetDll_inet_addr_entry)
REX_EXPORT(__imp__NetDll_socket, rex::kernel::xam::NetDll_socket_entry)
REX_EXPORT(__imp__NetDll_closesocket, rex::kernel::xam::NetDll_closesocket_entry)
REX_EXPORT(__imp__NetDll_shutdown, rex::kernel::xam::NetDll_shutdown_entry)
REX_EXPORT(__imp__NetDll_setsockopt, rex::kernel::xam::NetDll_setsockopt_entry)
REX_EXPORT(__imp__NetDll_ioctlsocket, rex::kernel::xam::NetDll_ioctlsocket_entry)
REX_EXPORT(__imp__NetDll_bind, rex::kernel::xam::NetDll_bind_entry)
REX_EXPORT(__imp__NetDll_connect, rex::kernel::xam::NetDll_connect_entry)
REX_EXPORT(__imp__NetDll_listen, rex::kernel::xam::NetDll_listen_entry)
REX_EXPORT(__imp__NetDll_accept, rex::kernel::xam::NetDll_accept_entry)
REX_EXPORT(__imp__NetDll_select, rex::kernel::xam::NetDll_select_entry)
REX_EXPORT(__imp__NetDll_recv, rex::kernel::xam::NetDll_recv_entry)
REX_EXPORT(__imp__NetDll_recvfrom, rex::kernel::xam::NetDll_recvfrom_entry)
REX_EXPORT(__imp__NetDll_send, rex::kernel::xam::NetDll_send_entry)
REX_EXPORT(__imp__NetDll_sendto, rex::kernel::xam::NetDll_sendto_entry)
REX_EXPORT(__imp__NetDll___WSAFDIsSet, rex::kernel::xam::NetDll___WSAFDIsSet_entry)
REX_EXPORT(__imp__NetDll_WSASetLastError, rex::kernel::xam::NetDll_WSASetLastError_entry)

REX_EXPORT_STUB(__imp__NetDll_UpnpActionCalculateWorkBufferSize);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpCleanup);
REX_EXPORT_STUB(__imp__NetDll_UpnpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpDoWork);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventGetCurrentState);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventUnsubscribe);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchGetDevices);
REX_EXPORT_STUB(__imp__NetDll_UpnpStartup);
REX_EXPORT_STUB(__imp__NetDll_WSACancelOverlappedIO);
REX_EXPORT(__imp__NetDll_WSAEventSelect, rex::kernel::xam::NetDll_WSAEventSelect_entry)
REX_EXPORT_STUB(__imp__NetDll_WSAGetOverlappedResult);
REX_EXPORT_STUB(__imp__NetDll_WSARecv);
REX_EXPORT_STUB(__imp__NetDll_WSASend);
REX_EXPORT_STUB(__imp__NetDll_WSAStartupEx);
REX_EXPORT_STUB(__imp__NetDll_XHttpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_XHttpConnect);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpDoWork);
REX_EXPORT_STUB(__imp__NetDll_XHttpGetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpen);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequestUsingMemory);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryAuthSchemes);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryHeaders);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpReadData);
REX_EXPORT_STUB(__imp__NetDll_XHttpReceiveResponse);
REX_EXPORT_STUB(__imp__NetDll_XHttpResetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpSendRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetCredentials);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetStatusCallback);
REX_EXPORT_STUB(__imp__NetDll_XHttpShutdown);
REX_EXPORT_STUB(__imp__NetDll_XHttpStartup);
REX_EXPORT_STUB(__imp__NetDll_XHttpWriteData);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseLookup);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseRelease);
REX_EXPORT_STUB(__imp__NetDll_XNetGetBroadcastVersionStatus);
REX_EXPORT_STUB(__imp__NetDll_XNetGetSystemLinkPort);
REX_EXPORT_STUB(__imp__NetDll_XNetGetXnAddrPlatform);
REX_EXPORT_STUB(__imp__NetDll_XNetInAddrToServer);
REX_EXPORT_STUB(__imp__NetDll_XNetQosGetListenStats);
REX_EXPORT(__imp__NetDll_XNetQosLookup, rex::kernel::xam::NetDll_XNetQosLookup_entry)
REX_EXPORT_STUB(__imp__NetDll_XNetReplaceKey);
REX_EXPORT_STUB(__imp__NetDll_XNetServerToInAddr);
REX_EXPORT_STUB(__imp__NetDll_XNetSetOpt);
REX_EXPORT_STUB(__imp__NetDll_XNetStartupEx);
REX_EXPORT_STUB(__imp__NetDll_XNetTsAddrToInAddr);
REX_EXPORT_STUB(__imp__NetDll_XNetUnregisterInAddr);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadContinue);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetParseTime);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetReceivedDataSize);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStart);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStop);
REX_EXPORT_STUB(__imp__NetDll_XnpCapture);
REX_EXPORT_STUB(__imp__NetDll_XnpConfig);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnP);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnPPortAndExternalAddr);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptRecv);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetExtendedReceiveCallback);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmit);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmitAsIp);
REX_EXPORT_STUB(__imp__NetDll_XnpGetActiveSocketList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetConfigStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpGetKeyList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetQosLookupList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetSecAssocList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetChallengeResponse);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetPState);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpNoteSystemTime);
REX_EXPORT_STUB(__imp__NetDll_XnpPersistTitleState);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetAggregateMeasurement);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetEntries);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryLoad);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistorySaveMeasurements);
REX_EXPORT_STUB(__imp__NetDll_XnpRegisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpReplaceKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpSetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpToolIpProxyInject);
REX_EXPORT_STUB(__imp__NetDll_XnpToolSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpUnregisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpUpdateConfigParams);
REX_EXPORT(__imp__NetDll_getpeername, rex::kernel::xam::NetDll_getpeername_entry)
REX_EXPORT(__imp__NetDll_getsockname, rex::kernel::xam::NetDll_getsockname_entry)
REX_EXPORT(__imp__NetDll_getsockopt, rex::kernel::xam::NetDll_getsockopt_entry)
