// Midnight Club: Los Angeles recomp — online multiplayer client. See mcla_online.h.

#include "xam/mcla_online.h"
#include "xam/mcla_online_protocol.h"

#include <rex/platform.h>  // REX_PLATFORM_WIN32

#include "xam/categories.h"
#include "xam/socket_object.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if REX_PLATFORM_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace rex::xam {

using namespace mclaonline;

OnlineSettings& GetOnlineSettings() {
  static OnlineSettings settings;
  return settings;
}

Result<void> RegisterOnlineSettings(ISettingsRegistry& registry) {
  OnlineSettings& settings = GetOnlineSettings();
  if (auto added = registry.Register(settings); !added) {
    return added;
  }
  // The names these settings had as v0.10 flat cvars. Config files, the
  // launcher and the command line written for an older build keep working;
  // each use of an old name adds one notice to the log.
  registry.AddAlias("mcla_online_enable", settings.enable.GetName());
  registry.AddAlias("mcla_online_server", settings.server.GetName());
  registry.AddAlias("mcla_online_port", settings.port.GetName());
  registry.AddAlias("mcla_online_connect_timeout_ms", settings.connect_timeout_ms.GetName());
  return {};
}

void UnregisterOnlineSettings(ISettingsRegistry& registry) {
  registry.Unregister(GetOnlineSettings());
}

OnlineClient& OnlineClient::Get() {
  static OnlineClient instance;
  return instance;
}

bool OnlineClient::Enabled() {
  return GetOnlineSettings().enable.Get();
}

uint32_t OnlineClient::NowMs() {
  using namespace std::chrono;
  return static_cast<uint32_t>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

OnlineClient::~OnlineClient() {
  Shutdown();
}

uint32_t OnlineClient::virtual_ip() {
  std::lock_guard<std::mutex> lock(mutex_);
  return virtual_ip_;
}

uint64_t OnlineClient::session_id() {
  std::lock_guard<std::mutex> lock(mutex_);
  return session_id_;
}

static void FillHeader(MsgHeader& h, MsgType type) {
  h.magic = kMagic;
  h.version = kProtocolVersion;
  h.type = static_cast<uint8_t>(type);
}

void OnlineClient::EnsureStarted() {
#if REX_PLATFORM_WIN32
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) {
      return;
    }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
      REXXAM_ERROR("[mcla-online] control socket() failed: {}", WSAGetLastError());
      return;
    }
    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(kServerPort);
    srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // Resolve the configured server. Accepts "host" or "host:port" (an IP or
    // hostname; e.g. a playit.gg tunnel "xxxx.ply.gg:2721"). Default 127.0.0.1
    // and port kServerPort (31000) for localhost play.
    {
      std::string host = GetOnlineSettings().server.Get();
      int cfg_port = GetOnlineSettings().port.Get();
      uint16_t port = (cfg_port > 0 && cfg_port < 65536) ? static_cast<uint16_t>(cfg_port)
                                                         : static_cast<uint16_t>(kServerPort);
      auto colon = host.rfind(':');
      if (colon != std::string::npos) {
        int p = std::atoi(host.c_str() + colon + 1);
        if (p > 0 && p < 65536) {
          port = static_cast<uint16_t>(p);
        }
        host = host.substr(0, colon);
      }
      srv.sin_port = htons(port);
      if (!host.empty()) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res) {
          srv.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
          freeaddrinfo(res);
        } else {
          REXXAM_ERROR("[mcla-online] could not resolve server '{}', using loopback", host);
        }
      }
      REXXAM_INFO("[mcla-online] server {}:{}", host, port);
    }
    if (connect(s, reinterpret_cast<sockaddr*>(&srv), sizeof(srv)) == SOCKET_ERROR) {
      REXXAM_ERROR("[mcla-online] control connect() failed: {}", WSAGetLastError());
      closesocket(s);
      return;
    }
    control_sock_ = static_cast<uintptr_t>(s);
    session_id_ = (static_cast<uint64_t>(GetCurrentProcessId()) << 32) ^
                  (GetTickCount64() & 0xFFFFFFFFull) ^ 0x4D434C41ull;  // MCLA Magic
    started_ = true;
    run_ = true;
    connection_checked_ = false;
    server_online_ = false;
    startup_time_ms_ = NowMs();
  }
  recv_thread_ = std::thread([this] { RecvLoop(); });
  heartbeat_thread_ = std::thread([this] { HeartbeatLoop(); });
  {
    // Request a virtual ip right away (don't wait for the first ~5s heartbeat) so
    // XNetGetTitleXnAddr can hand the title its real vip before it builds its
    // advertised session XNADDR.
    std::lock_guard<std::mutex> lock(mutex_);
    SendRegister(hosting_);
  }
  REXXAM_INFO("[mcla-online] client started, session={:016X}", session_id_);
#endif
}

void OnlineClient::Shutdown() {
#if REX_PLATFORM_WIN32
  run_ = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (control_sock_ != ~uintptr_t(0)) {
      closesocket(static_cast<SOCKET>(control_sock_));  // unblocks RecvLoop
      control_sock_ = ~uintptr_t(0);
    }
    started_ = false;
  }
  if (recv_thread_.joinable()) {
    recv_thread_.join();
  }
  if (heartbeat_thread_.joinable()) {
    heartbeat_thread_.join();
  }
#endif
}

bool OnlineClient::SendRaw(const void* data, int len) {
#if REX_PLATFORM_WIN32
  if (control_sock_ == ~uintptr_t(0)) {
    return false;
  }
  return send(static_cast<SOCKET>(control_sock_), reinterpret_cast<const char*>(data), len, 0) ==
         len;
#else
  (void)data;
  (void)len;
  return false;
#endif
}

// Fills the session block shared by kRegister and kHeartbeat. Caller must hold
// mutex_ (every call site already does).
void OnlineClient::FillSession(SessionInfo& s, bool is_host) const {
  s.session_id = session_id_;
  s.virtual_ip = virtual_ip_;
  s.host_port = game_port_;
  s.gamemode = host_gamemode_;
  s.map = host_map_;
  s.max_players = host_max_;
  // Prefer the real MaxPlayers the game chose (session property 0x1000000F,
  // 2..16) over the placeholder, so the relay/lobby advertise the true session
  // size instead of a fixed 4 (which capped joins and caused "no free slot").
  for (uint16_t i = 0; i < prop_count_; ++i) {
    if (props_[i].id == 0x1000000F) {
      uint32_t v = props_[i].value;
      if (v >= 2 && v <= 16) {
        s.max_players = static_cast<uint8_t>(v);
      }
      break;
    }
  }
  s.cur_players = host_cur_;
  s.is_host = is_host ? 1 : 0;
  std::snprintf(s.name, sizeof(s.name), "%s", host_name_[0] ? host_name_ : "MCLA");
  std::snprintf(s.gamertag, sizeof(s.gamertag), "%s", gamertag_[0] ? gamertag_ : "Player");
  s.xuid = xuid_;
  s.prop_count = prop_count_;
  std::memcpy(s.props, props_, sizeof(s.props));
}

void OnlineClient::SetSessionProps(const std::vector<SessionProp>& props) {
  std::lock_guard<std::mutex> lock(mutex_);
  uint16_t n = static_cast<uint16_t>(std::min<size_t>(props.size(), kMaxSessionProps));
  for (uint16_t i = 0; i < n; ++i) {
    props_[i] = props[i];
  }
  for (uint16_t i = n; i < kMaxSessionProps; ++i) {
    props_[i] = SessionProp{};
  }
  prop_count_ = n;
}

void OnlineClient::UpdateSessionProp(uint32_t id, uint32_t value) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Update in place if we already carry this id.
  for (uint16_t i = 0; i < prop_count_; ++i) {
    if (props_[i].id == id) {
      props_[i].value = value;
      return;
    }
  }
  // Otherwise append (bounded).
  if (prop_count_ < kMaxSessionProps) {
    props_[prop_count_].id = id;
    props_[prop_count_].value = value;
    ++prop_count_;
  }
}

void OnlineClient::SetGamertag(const char* gamertag) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!gamertag || !gamertag[0]) {
    return;
  }
  // Truncates to 15 chars + NUL, which is already the Xbox gamertag limit.
  std::snprintf(gamertag_, sizeof(gamertag_), "%s", gamertag);
}

void OnlineClient::SetXuid(uint64_t xuid) {
  std::lock_guard<std::mutex> lock(mutex_);
  xuid_ = xuid;
}

void OnlineClient::SendRegister(bool is_host) {
  RegisterMsg msg{};
  FillHeader(msg.hdr, MsgType::kRegister);
  FillSession(msg.session, is_host);
  SendRaw(&msg, sizeof(msg));
}

void OnlineClient::Reprobe() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_ || control_sock_ == ~uintptr_t(0)) {
    return;
  }
  SendRegister(hosting_);
}

void OnlineClient::OnBind(SocketObject* match_socket, uint16_t port) {
  EnsureStarted();
  std::lock_guard<std::mutex> lock(mutex_);
  sockets_by_port_[port] = match_socket;
  if (port != 1000 && port != 1001) {
    fallback_socket_ = match_socket;
  }
  if (port != 1001) {
    game_port_ = port;  // last bound; advertised as the host port
  }
  SendRegister(hosting_);
  REXXAM_INFO("[mcla-online] peer registered (port {})", port);
}

void OnlineClient::OnCloseSocket(SocketObject* socket) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (fallback_socket_ == socket) {
    fallback_socket_ = nullptr;
  }
  for (auto it = sockets_by_port_.begin(); it != sockets_by_port_.end();) {
    if (it->second == socket) {
      it = sockets_by_port_.erase(it);
    } else {
      ++it;
    }
  }
}

void OnlineClient::OnSocketsShutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  fallback_socket_ = nullptr;
  sockets_by_port_.clear();
}

void OnlineClient::RegisterHost(uint32_t gamemode, uint32_t map, uint8_t max_players,
                                uint8_t cur_players, const char* name) {
  EnsureStarted();
  std::lock_guard<std::mutex> lock(mutex_);
  hosting_ = true;
  host_gamemode_ = gamemode;
  host_map_ = map;
  host_max_ = max_players;
  host_cur_ = cur_players;
  std::snprintf(host_name_, sizeof(host_name_), "%s", name ? name : "MCLA");
  SendRegister(true);
  REXXAM_INFO("[mcla-online] REGISTER host port={} mode={} {}/{}", game_port_, gamemode,
              cur_players, max_players);
}

void OnlineClient::Unregister() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (control_sock_ == ~uintptr_t(0)) {
    return;
  }
  RegisterMsg msg{};
  FillHeader(msg.hdr, MsgType::kUnregister);
  msg.session.session_id = session_id_;
  SendRaw(&msg, sizeof(msg));
  hosting_ = false;
}

void OnlineClient::SendQuery() {
  QueryMsg msg{};
  FillHeader(msg.hdr, MsgType::kQuery);
  SendRaw(&msg, sizeof(msg));
}

std::vector<SessionInfo> OnlineClient::QuerySessions(int timeout_ms) {
#if REX_PLATFORM_WIN32
  EnsureStarted();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_ready_ = false;
    SendQuery();
  }
  {
    std::unique_lock<std::mutex> lock(mutex_);
    sessions_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                          [this] { return sessions_ready_; });
    return sessions_;
  }
#else
  (void)timeout_ms;
  return {};
#endif
}

void OnlineClient::SendRelay(uint32_t dest_vip, uint16_t port, const uint8_t* data, uint16_t len) {
#if REX_PLATFORM_WIN32
  if (len > kMaxInner) {
    len = kMaxInner;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (control_sock_ == ~uintptr_t(0)) {
    return;
  }
  uint8_t buf[sizeof(RelayMsg) + kMaxInner];
  auto* rm = reinterpret_cast<RelayMsg*>(buf);
  FillHeader(rm->hdr, MsgType::kRelay);
  rm->addr = dest_vip;  // 0 = broadcast
  rm->src_port = port;  // unified deliver-to + source port
  rm->inner_len = len;
  std::memcpy(buf + sizeof(RelayMsg), data, len);
  SendRaw(buf, static_cast<int>(sizeof(RelayMsg) + len));
#else
  (void)dest_vip;
  (void)port;
  (void)data;
  (void)len;
#endif
}

void OnlineClient::RecvLoop() {
#if REX_PLATFORM_WIN32
  uint8_t buf[sizeof(RelayMsg) + kMaxInner + 64];
  while (run_) {
    SOCKET s;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (control_sock_ == ~uintptr_t(0)) {
        break;
      }
      s = static_cast<SOCKET>(control_sock_);
    }
    int n = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
    if (n < static_cast<int>(sizeof(MsgHeader))) {
      if (n <= 0 && !run_) {
        break;
      }
      continue;
    }
    auto* hdr = reinterpret_cast<MsgHeader*>(buf);
    if (hdr->magic != kMagic || hdr->version != kProtocolVersion) {
      continue;
    }
    switch (static_cast<MsgType>(hdr->type)) {
      case MsgType::kRegisterAck: {
        if (n >= static_cast<int>(sizeof(RegisterMsg))) {
          auto* m = reinterpret_cast<RegisterMsg*>(buf);
          std::lock_guard<std::mutex> lock(mutex_);
          server_online_ = true;
          connection_checked_ = true;
          if (virtual_ip_ == 0) {
            virtual_ip_ = m->session.virtual_ip;
            REXXAM_INFO("[mcla-online] assigned virtual ip {}.{}.{}.{}", (virtual_ip_ >> 24) & 0xFF,
                        (virtual_ip_ >> 16) & 0xFF, (virtual_ip_ >> 8) & 0xFF, virtual_ip_ & 0xFF);
          }
        }
        break;
      }
      case MsgType::kPing: {
        // Server RTT probe: echo the token straight back as a kPong.
        if (n >= static_cast<int>(sizeof(PingMsg))) {
          auto* pm = reinterpret_cast<PingMsg*>(buf);
          pm->hdr.type = static_cast<uint8_t>(MsgType::kPong);
          SendRaw(pm, sizeof(PingMsg));
        }
        break;
      }
      case MsgType::kList: {
        if (n >= static_cast<int>(sizeof(ListMsg))) {
          auto* lm = reinterpret_cast<ListMsg*>(buf);
          uint16_t count = lm->count;
          int expected = static_cast<int>(sizeof(ListMsg) + count * sizeof(SessionInfo));
          if (n >= expected) {
            std::lock_guard<std::mutex> lock(mutex_);
            sessions_.clear();
            auto* entries = reinterpret_cast<SessionInfo*>(buf + sizeof(ListMsg));
            for (uint16_t i = 0; i < count; ++i) {
              sessions_.push_back(entries[i]);
            }
            sessions_ready_ = true;
            REXXAM_INFO("[mcla-online] received session list: {} sessions", count);
            sessions_cv_.notify_all();
          }
        }
        break;
      }
      case MsgType::kRelayDeliver: {
        if (n >= static_cast<int>(sizeof(RelayMsg))) {
          auto* rm = reinterpret_cast<RelayMsg*>(buf);
          uint16_t inner_len = rm->inner_len;
          if (inner_len > kMaxInner || n < static_cast<int>(sizeof(RelayMsg)) + inner_len) {
            break;
          }
          // Queue the inner packet onto the matching socket so WSARecvFrom
          // returns it with the sender's virtual ip as the source address. The
          // lookup and the delivery both run under mutex_: closesocket and the
          // title's socket shutdown remove the socket under the same lock, so
          // the socket cannot be destroyed while it is being written to.
          const uint32_t src_vip = rm->addr;
          const uint16_t src_port = rm->src_port;
          std::lock_guard<std::mutex> lock(mutex_);
          SocketObject* sock = nullptr;
          auto it = sockets_by_port_.find(src_port);
          if (it != sockets_by_port_.end()) {
            sock = it->second;
          } else {
            sock = fallback_socket_;
          }
          if (sock) {
            sock->QueuePacket(src_vip, src_port, buf + sizeof(RelayMsg), inner_len);
          }
        }
        break;
      }
      default:
        break;
    }
  }
#endif
}

void OnlineClient::HeartbeatLoop() {
#if REX_PLATFORM_WIN32
  while (run_) {
    for (int i = 0; i < 50 && run_; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));  // ~5s
    }
    if (!run_) {
      break;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (control_sock_ != ~uintptr_t(0)) {
      RegisterMsg msg{};
      FillHeader(msg.hdr, MsgType::kHeartbeat);
      FillSession(msg.session, hosting_);
      SendRaw(&msg, sizeof(msg));
    }
  }
#endif
}

uint16_t OnlineClient::game_port() {
  std::lock_guard<std::mutex> lock(mutex_);
  return game_port_;
}

bool OnlineClient::connection_checked() {
  std::lock_guard<std::mutex> lock(mutex_);
  return connection_checked_;
}

bool OnlineClient::server_online() {
  std::lock_guard<std::mutex> lock(mutex_);
  return server_online_;
}

bool OnlineClient::advertise_as_host() {
  std::lock_guard<std::mutex> lock(mutex_);
  return advertise_as_host_;
}

void OnlineClient::set_advertise_as_host(bool advertise) {
  std::lock_guard<std::mutex> lock(mutex_);
  advertise_as_host_ = advertise;
  if (!advertise && hosting_) {
    // We hosted earlier in this process and have now joined someone else's
    // session. Skipping RegisterHost is not enough: the heartbeat keeps
    // resending is_host from hosting_, so the server would go on advertising a
    // session we already left. Clear it and tell the server right away.
    hosting_ = false;
    SendRegister(false);
  }
}

uint32_t OnlineClient::startup_time_ms() {
  std::lock_guard<std::mutex> lock(mutex_);
  return startup_time_ms_;
}

void OnlineClient::set_connection_checked(bool checked) {
  std::lock_guard<std::mutex> lock(mutex_);
  connection_checked_ = checked;
}

}  // namespace rex::xam
