// Midnight Club: Los Angeles recomp — online multiplayer wire protocol.
// Plain UDP, little-endian, packed structs.

#pragma once
#include <cstdint>

namespace rex::xam::mclaonline {

constexpr uint32_t kMagic = 0x3230434D;  // "MC02"
constexpr uint16_t kServerPort = 31000;
// Bumping is mandatory whenever SessionInfo changes size: peers and server
// validate MsgHeader::version and drop mismatches, which is what keeps an old
// build from misreading the struct.
//   v3 added SessionInfo::gamertag.
//   v4 added SessionInfo::props (host session properties/contexts) so the
//      joiner can rebuild the event definition (content mask, vehicle class,
//      max players, ...). Without it XSessionSearchEx returns cProperties=0 and
//      the joiner sees an empty event -> wrong DLC/vehicle-legality prompts.
//   v5 added SessionInfo::xuid, and kPing/kPong for server-measured RTT.
constexpr uint32_t kProtocolVersion = 5;
constexpr uint32_t kSessionTimeoutSec = 15;

enum class MsgType : uint8_t {
  kRegister = 1,     // peer -> server: announce/refresh (host session OR just a peer)
  kUnregister = 2,
  kHeartbeat = 3,
  kQuery = 4,        // client -> server: list advertised host sessions
  kList = 5,         // server -> client
  kRegisterAck = 6,  // server -> peer: assigns virtual_ip (in session.virtual_ip)
  kRelay = 7,        // peer -> server: relay an inner game packet
  kRelayDeliver = 8, // server -> peer: a relayed inner game packet
  kPing = 9,         // server -> peer: echo the token back (RTT probe)
  kPong = 10,        // peer -> server: token echoed
};

#pragma pack(push, 1)

struct MsgHeader {
  uint32_t magic;
  uint32_t version;
  uint8_t type;
};

// One host session property or context, as {id, 32-bit value}. This matches
// both XUSER_CONTEXT (id + DWORD value) and the low-nibble entry stride of the
// game's own session-settings blob, so a joiner can feed these straight back.
// 64-bit properties (e.g. HostUID) are intentionally not carried; only the ones
// that gate the event definition (Content0..7, VehicleClass, MaxPlayers,
// GameMode/GameType, ...) matter and they are all 32-bit.
struct SessionProp {
  uint32_t id;
  uint32_t value;
};

// Max session properties carried per session. The guest store holds 32 records;
// 40 leaves headroom without bloating the datagram (40*8 = 320 bytes).
constexpr uint16_t kMaxSessionProps = 40;

struct SessionInfo {
  uint64_t session_id;
  uint32_t virtual_ip;  // server-assigned peer identity (host order); 0 until acked
  uint32_t host_ip;     // real source IP (network order); server fills it
  uint16_t host_port;   // host's game UDP port (nominal)
  uint32_t gamemode;
  uint32_t map;
  uint8_t max_players;
  uint8_t cur_players;
  uint8_t is_host;  // 1 = advertised host session, 0 = plain peer
  char name[32];
  // Profile gamertag of this peer, NUL terminated. Sent by every peer, host or
  // not, so the server can identify who is connected (name[] is the session
  // name, which only a host sets). Xbox gamertags are at most 15 chars.
  char gamertag[16];
  // Profile XUID of this peer (0 if not signed in / unavailable). Identifies a
  // user across reconnects and IP changes; used by the admin panel.
  uint64_t xuid;
  // Host session properties/contexts (see SessionProp). prop_count entries are
  // valid; the rest are zero. Empty (prop_count == 0) for plain peers and for
  // hosts that had not populated their property store yet.
  uint16_t prop_count;
  SessionProp props[kMaxSessionProps];
};

// RTT probe. Server sends kPing with an opaque token; peer echoes it back in a
// kPong. Server matches the token to the send time to compute round-trip.
struct PingMsg {
  MsgHeader hdr;
  uint64_t token;
};

struct RegisterMsg {
  MsgHeader hdr;
  SessionInfo session;
};

struct QueryMsg {
  MsgHeader hdr;
};

struct ListMsg {
  MsgHeader hdr;
  uint16_t count;
  // SessionInfo entries[count] follow.
};

struct RelayMsg {
  MsgHeader hdr;
  uint32_t addr;        // dest vip (kRelay) or src vip (kRelayDeliver), host order
  uint16_t src_port;    // sender's nominal game port
  uint16_t inner_len;
  // uint8_t inner[inner_len] follows.
};
constexpr uint16_t kMaxInner = 1408;

#pragma pack(pop)

}  // namespace rex::xam::mclaonline
