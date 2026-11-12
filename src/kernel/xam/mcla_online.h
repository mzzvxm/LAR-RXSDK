// Midnight Club: Los Angeles recomp — online multiplayer client.
//
// Bridges the game's emulated XNet (xam_net.cpp) to the localhost matchmaker +
// relay. The game does LAN-broadcast discovery on port 1001; we route those
// broadcasts (and the unicast replies) through the server so they cross
// processes/machines, and deliver them into the match socket's packet queue.
// Each instance gets a server-assigned VIRTUAL IP used as its peer identity.
//
// Gated entirely by the `mcla_online_enable` cvar.

#pragma once

#include "mcla_online_protocol.h"

#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rex::system {
class XSocket;
}

namespace rex::kernel::xam {

class OnlineClient {
 public:
  static OnlineClient& Get();
  static bool Enabled();

  void EnsureStarted();
  void Shutdown();

  // From bind(): register this instance as a peer (assigns a virtual ip) and
  // remember the match socket so relayed packets can be queued onto it.
  void OnBind(rex::system::XSocket* match_socket, uint16_t port);
  void OnCloseSocket(rex::system::XSocket* socket);

  // Host advertises a session (from XNetQosListen).
  void RegisterHost(uint32_t gamemode, uint32_t map, uint8_t max_players, uint8_t cur_players,
                    const char* name);
  void Unregister();

  // Profile gamertag reported to the server with every register/heartbeat.
  // Cached here because the heartbeat thread must not touch the kernel state;
  // callers on a guest thread push it in.
  void SetGamertag(const char* gamertag);

  // Profile XUID reported to the server (admin panel identity). Cached because
  // the heartbeat thread must not touch kernel state; a guest thread pushes it.
  void SetXuid(uint64_t xuid);

  // Host session properties/contexts to advertise (the event definition). A
  // guest thread reads them from the property store and pushes them here; the
  // heartbeat thread just resends the cached copy. Empty vector clears them.
  void SetSessionProps(const std::vector<mclaonline::SessionProp>& props);

  // Upsert a single session property/context as the game sets it (from
  // XGIUserSetPropertyEx / XGIUserSetContextEx). This captures real values at
  // the moment they are set, unlike a one-shot store read which runs before the
  // game populates the store. Called on a guest thread.
  void UpdateSessionProp(uint32_t id, uint32_t value);

  // This instance's assigned virtual ip (host order); 0 until the server acks.
  uint32_t virtual_ip();
  uint64_t session_id();
  uint16_t game_port();

  bool connection_checked();
  bool server_online();

  // False once the game has created a session as a *guest* (XSessionCreate
  // without XSESSION_CREATE_HOST). A guest must not advertise a session of its
  // own to the relay, or the lobby list shows a phantom host and peers can pick
  // the wrong machine to point their netSession at. Defaults to true so the
  // ordinary host path is unchanged.
  bool advertise_as_host();
  void set_advertise_as_host(bool advertise);
  uint32_t startup_time_ms();
  void set_connection_checked(bool checked);

  // Resend the register/probe to the relay server. Used while waiting for the
  // initial ack so a dropped first datagram (common over the internet/VPN) gets
  // retried instead of falsely reporting the server offline.
  void Reprobe();

  // Query the relay server for advertised sessions. Blocks up to timeout_ms
  // waiting for the server's kList response. Returns the session list.
  std::vector<mclaonline::SessionInfo> QuerySessions(int timeout_ms = 500);

  // Relay a game packet sent from a socket bound to `port`. dest_vip == 0 =>
  // broadcast to all peers. Delivered to the peer's socket on the same port.
  void SendRelay(uint32_t dest_vip, uint16_t port, const uint8_t* data, uint16_t len);

 private:
  OnlineClient() = default;
  ~OnlineClient();

  void RecvLoop();
  void HeartbeatLoop();
  void FillSession(mclaonline::SessionInfo& s, bool is_host) const;
  void SendRegister(bool is_host);
  void SendQuery();
  bool SendRaw(const void* data, int len);

  std::mutex mutex_;
  uintptr_t control_sock_ = ~uintptr_t(0);
  bool started_ = false;
  bool connection_checked_ = false;
  bool server_online_ = false;
  uint32_t startup_time_ms_ = 0;
  uint64_t session_id_ = 0;
  uint32_t virtual_ip_ = 0;
  uint16_t game_port_ = 0;

  bool hosting_ = false;
  bool advertise_as_host_ = true;
  uint32_t host_gamemode_ = 0;
  uint32_t host_map_ = 0;
  uint8_t host_max_ = 4;
  uint8_t host_cur_ = 1;
  char host_name_[32] = {};
  char gamertag_[16] = {};
  uint64_t xuid_ = 0;
  uint16_t prop_count_ = 0;
  mclaonline::SessionProp props_[mclaonline::kMaxSessionProps] = {};

  // All match sockets, keyed by bound port (1001 = discovery UDP, 1000 = game
  // VDP). Relayed packets are queued onto the socket matching the dest port.
  std::unordered_map<uint16_t, rex::system::XSocket*> sockets_by_port_;
  rex::system::XSocket* fallback_socket_ = nullptr;

  // Session list received from server kList response.
  std::vector<mclaonline::SessionInfo> sessions_;
  std::condition_variable sessions_cv_;
  bool sessions_ready_ = false;

  std::thread recv_thread_;
  std::thread heartbeat_thread_;
  volatile bool run_ = false;
};

}  // namespace rex::kernel::xam
