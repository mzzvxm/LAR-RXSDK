/**
 * @file        tests/unit/kernel/xma_context_frames_test.cpp
 * @brief       How an XMA context finds the frames of a packet and the next packet of its stream
 *
 * 8a72fa25 stopped the decoder dropping a whole frame every 10 to 20 seconds of a long stream
 * (changes of Xenia Canary and Xenia Edge). Three of them are in the packet navigation and run on
 * synthetic packets here, with no decoder and no guest memory:
 *
 *   - GetPacketInfo counts a frame whose 15 bit header runs into the next packet, with size 0, so
 *     that the frame before it is not taken for the packet's last and the crossing frame skipped
 *     (aee0871dd);
 *   - GetPacketInfo resolves an offset one bit short of a frame start to that frame, for a loop
 *     start that is not on the boundary (5dd1cdbbf);
 *   - GetNextPacketReadOffset follows the skip count of a packet in which no frame starts, to the
 *     next packet of the same stream (7015473a7).
 *
 * The other two changes are in XMAContext::Work: it does not leave its loop while a frame is only
 * partly handed out (6bef1fbed), and a pass that moves nothing ends the loop (ade7e610b). Both need
 * a context in guest memory and a stream FFmpeg decodes, and are not tested here.
 * CHECK: that decode-level test is still missing; so is the comparison with FFmpeg's decode of a
 * real stream, as in the original measurement (6 to 11 frames lost in 95 s before the fix).
 *
 * The two functions are private. The test reaches them through a pointer to the member that an
 * explicit instantiation obtains, which the language lets name a private member, so the class
 * needs no friend.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <array>
#include <vector>

#include "kernel/xma/context.h"
#include "kernel/xma/packet.h"
#include <rex/memory/stream.h>

using rex::kernel::PacketFrame;
using rex::kernel::XMAContext;

namespace {

// Names a private member through the friend function that an explicit instantiation defines.
template <typename Tag, typename Tag::type Member>
struct Access {
  friend typename Tag::type Get(Tag) { return Member; }
};

struct GetPacketInfoTag {
  using type = PacketFrame (XMAContext::*)(rex::u8*, rex::u32);
  friend type Get(GetPacketInfoTag);
};
struct GetNextPacketReadOffsetTag {
  using type = rex::u32 (XMAContext::*)(rex::u8*, rex::u32, rex::u32);
  friend type Get(GetNextPacketReadOffsetTag);
};

template struct Access<GetPacketInfoTag, &XMAContext::GetPacketInfo>;
template struct Access<GetNextPacketReadOffsetTag, &XMAContext::GetNextPacketReadOffset>;

constexpr rex::u32 kBitsPerPacket = XMAContext::kBitsPerPacket;
constexpr rex::u32 kBytesPerPacket = XMAContext::kBytesPerPacket;
constexpr rex::u32 kHeaderBits = XMAContext::kBitsPerPacketHeader;
constexpr rex::u32 kMaxFrameSizeInBits = XMAContext::kMaxFrameSizeInBits;
// The bit reader loads 8 bytes at any bit offset, so a packet at the end of a buffer needs some.
constexpr size_t kReadPadding = 16;

// A buffer of packets. Every packet starts as zeros: no frame size, metadata 0, skip count 0.
class Packets {
 public:
  explicit Packets(rex::u32 count) : bytes_(size_t{count} * kBytesPerPacket + kReadPadding) {}

  rex::u8* Packet(rex::u32 index) { return bytes_.data() + size_t{index} * kBytesPerPacket; }

  // The 4 byte header: the count of frames that begin in the packet (6 bits), the offset of the
  // first of them from the end of the header (15 bits), the metadata (3 bits, 1 is XMA2) and the
  // skip count to the next packet of the stream.
  void SetHeader(rex::u32 index, rex::u32 frame_count, rex::u32 first_frame_offset,
                 rex::u32 metadata, rex::u32 skip_count) {
    rex::u8* packet = Packet(index);
    packet[0] = static_cast<rex::u8>(frame_count << 2 | (first_frame_offset >> 13 & 0x3));
    packet[1] = static_cast<rex::u8>(first_frame_offset >> 5);
    packet[2] = static_cast<rex::u8>((first_frame_offset & 0x1F) << 3 | (metadata & 0x7));
    packet[3] = static_cast<rex::u8>(skip_count);
  }

  // A frame of size bits at bit_offset of the packet: the 15 bit size, which counts the header
  // and the last bit, and the last bit, set when another frame follows in the same packet.
  void SetFrame(rex::u32 index, rex::u32 bit_offset, rex::u32 size, bool another_follows) {
    rex::memory::BitStream stream(Packet(index), kBitsPerPacket);
    stream.SetOffset(bit_offset);
    stream.Write(size, 15);
    stream.SetOffset(bit_offset + size - 1);
    stream.Write(another_follows ? 1 : 0, 1);
  }

 private:
  std::vector<rex::u8> bytes_;
};

PacketFrame PacketInfo(rex::u8* packet, rex::u32 frame_offset) {
  XMAContext context;
  return (context.*Get(GetPacketInfoTag{}))(packet, frame_offset);
}

rex::u32 NextPacketReadOffset(Packets& packets, rex::u32 next_packet_index, rex::u32 packet_count) {
  XMAContext context;
  return (context.*Get(GetNextPacketReadOffsetTag{}))(packets.Packet(0), next_packet_index,
                                                      packet_count);
}

// Three frames in packet 0 at bits 32, 432 and 932, of 400, 500 and 600 bits.
Packets ThreeFrames() {
  Packets packets(1);
  packets.SetHeader(0, 3, 0, 1, 0);
  packets.SetFrame(0, 32, 400, true);
  packets.SetFrame(0, 432, 500, true);
  packets.SetFrame(0, 932, 600, false);
  return packets;
}

}  // namespace

REX_TEST_CASE("The packet header fields read back as written", "[kernel][xma]") {
  Packets packets(1);
  packets.SetHeader(0, 37, 0x1ABC, 1, 9);
  const rex::u8* packet = packets.Packet(0);
  CHECK(rex::kernel::xma::GetPacketFrameCount(packet) == 37);
  CHECK(rex::kernel::xma::GetPacketFrameOffset(packet) == 0x1ABC + kHeaderBits);
  CHECK(rex::kernel::xma::GetPacketMetadata(packet) == 1);
  CHECK(rex::kernel::xma::IsPacketXMA2Type(packet));
  CHECK(rex::kernel::xma::GetPacketSkipCount(packet) == 9);
}

REX_TEST_CASE("GetPacketInfo counts the frames of a packet and finds the one at an offset",
              "[kernel][xma]") {
  auto packets = ThreeFrames();
  rex::u8* packet = packets.Packet(0);

  const PacketFrame second = PacketInfo(packet, 432);
  CHECK(second.frame_count == 3);
  CHECK(second.current_frame == 1);
  CHECK(second.current_frame_size == 500);
  CHECK(second.current_frame_offset == 432);
  CHECK_FALSE(second.IsLastFrameInPacket());

  const PacketFrame last = PacketInfo(packet, 932);
  CHECK(last.frame_count == 3);
  CHECK(last.current_frame == 2);
  CHECK(last.current_frame_size == 600);
  CHECK(last.IsLastFrameInPacket());

  const PacketFrame first = PacketInfo(packet, 32);
  CHECK(first.current_frame == 0);
  CHECK(first.current_frame_size == 400);
  CHECK_FALSE(first.IsLastFrameInPacket());
}

REX_TEST_CASE("GetPacketInfo gives the rest of a frame that began in the previous packet",
              "[kernel][xma]") {
  // The first frame starts 100 bits after the header. What is before it is the tail of the frame
  // that crossed into this packet: frame 0, with the distance to the first frame as its size.
  Packets packets(1);
  packets.SetHeader(0, 2, 100, 1, 0);
  packets.SetFrame(0, kHeaderBits + 100, 400, true);
  packets.SetFrame(0, kHeaderBits + 500, 500, false);

  const PacketFrame tail = PacketInfo(packets.Packet(0), kHeaderBits);
  CHECK(tail.current_frame == 0);
  CHECK(tail.current_frame_size == 100);
  CHECK(tail.frame_count == 2);
  CHECK_FALSE(tail.IsLastFrameInPacket());
}

namespace {

// The first frame ends 10 bits before the end of the packet and says another follows: the next
// frame's 15 bit header has only those 10 bits in this packet. Metadata 0 is a packet whose header
// has no frame count to fall back on, so the count comes from the frames alone.
void CheckCrossingHeader(rex::u32 metadata) {
  constexpr rex::u32 kCrossingAt = kBitsPerPacket - 10;
  Packets packets(1);
  packets.SetHeader(0, metadata == 1 ? 2 : 0, 0, metadata, 0);
  packets.SetFrame(0, 32, kCrossingAt - 32, true);

  const PacketFrame crossing = PacketInfo(packets.Packet(0), kCrossingAt);
  CHECK(crossing.frame_count == 2);
  CHECK(crossing.current_frame == 1);
  CHECK(crossing.current_frame_size == 0);  // the size is not readable: the split header path
  CHECK(crossing.current_frame_offset == kCrossingAt);
  CHECK(crossing.IsLastFrameInPacket());

  // The frame before it is not the last of the packet. Counted without the crossing frame, it
  // would be, and the crossing frame would be skipped.
  const PacketFrame before = PacketInfo(packets.Packet(0), 32);
  CHECK(before.frame_count == 2);
  CHECK(before.current_frame == 0);
  CHECK(before.current_frame_size == kCrossingAt - 32);
  CHECK_FALSE(before.IsLastFrameInPacket());
}

}  // namespace

REX_TEST_CASE("GetPacketInfo counts a frame whose header crosses into the next packet",
              "[kernel][xma]") {
  CheckCrossingHeader(0);
}

REX_TEST_CASE("The crossing frame is counted in an XMA2 packet too", "[kernel][xma]") {
  CheckCrossingHeader(1);
}

REX_TEST_CASE("GetPacketInfo does not count a frame that ends exactly at the end of the packet",
              "[kernel][xma]") {
  Packets packets(1);
  packets.SetHeader(0, 1, 0, 1, 0);
  packets.SetFrame(0, 32, kBitsPerPacket - 32, true);

  const PacketFrame only = PacketInfo(packets.Packet(0), 32);
  CHECK(only.frame_count == 1);
  CHECK(only.current_frame == 0);
  CHECK(only.IsLastFrameInPacket());
}

REX_TEST_CASE("GetPacketInfo takes the frame count of an XMA2 packet header when it is higher",
              "[kernel][xma]") {
  // Two frames parsed, five that begin in the packet by its header, the rest not readable here.
  Packets packets(1);
  packets.SetHeader(0, 5, 0, 1, 0);
  packets.SetFrame(0, 32, 400, true);
  packets.SetFrame(0, 432, 500, false);
  const PacketFrame info = PacketInfo(packets.Packet(0), 432);
  CHECK(info.frame_count == 5);
  CHECK(info.current_frame == 1);
}

REX_TEST_CASE("GetPacketInfo resolves an offset one bit short of a frame to that frame",
              "[kernel][xma]") {
  auto packets = ThreeFrames();
  rex::u8* packet = packets.Packet(0);

  // A loop start one bit before the second frame, then one bit after the first frame's start.
  CHECK(PacketInfo(packet, 431).current_frame_offset == 432);
  CHECK(PacketInfo(packet, 33).current_frame_offset == 432);
  CHECK(PacketInfo(packet, 931).current_frame_offset == 932);
  // On a frame start it is that frame, and the frame is the current one.
  const PacketFrame exact = PacketInfo(packet, 432);
  CHECK(exact.current_frame_offset == 432);
  CHECK(exact.current_frame_size == 500);
  // Short of a frame the size is not claimed: only the offset is resolved.
  CHECK(PacketInfo(packet, 431).current_frame_size == 0);
}

REX_TEST_CASE("GetPacketInfo keeps an offset after the last frame start", "[kernel][xma]") {
  auto packets = ThreeFrames();
  const PacketFrame after = PacketInfo(packets.Packet(0), 1500);
  CHECK(after.current_frame_offset == 1500);
  CHECK(after.current_frame_size == 0);
  CHECK(after.frame_count == 3);
}

REX_TEST_CASE("GetNextPacketReadOffset returns the first frame of the next packet",
              "[kernel][xma]") {
  Packets packets(3);
  packets.SetHeader(0, 1, 100, 1, 0);
  packets.SetHeader(1, 1, 0, 1, 0);
  CHECK(NextPacketReadOffset(packets, 0, 3) == 0 * kBitsPerPacket + 100 + kHeaderBits);
  CHECK(NextPacketReadOffset(packets, 1, 3) == 1 * kBitsPerPacket + 0 + kHeaderBits);
}

REX_TEST_CASE("GetNextPacketReadOffset follows the skip count of a packet without a frame start",
              "[kernel][xma]") {
  // Packet 0 only continues a frame from before it (its first frame offset is past the largest
  // frame), and its skip count of 1 leads over packet 1 to packet 2. Packet 1 has a frame start,
  // which taking the next packet would find instead.
  constexpr rex::u32 kNoFrame = 0x7FFF;
  Packets packets(4);
  packets.SetHeader(0, 0, kNoFrame, 1, 1);
  packets.SetHeader(1, 1, 10, 1, 0);
  packets.SetHeader(2, 1, 50, 1, 0);
  CHECK(NextPacketReadOffset(packets, 0, 4) == 2 * kBitsPerPacket + 50 + kHeaderBits);

  // A skip count of 0 leads to the next packet.
  packets.SetHeader(0, 0, kNoFrame, 1, 0);
  CHECK(NextPacketReadOffset(packets, 0, 4) == 1 * kBitsPerPacket + 10 + kHeaderBits);

  // Two packets without a frame in a row are followed one after the other.
  packets.SetHeader(0, 0, kNoFrame, 1, 0);
  packets.SetHeader(1, 0, kNoFrame, 1, 1);
  packets.SetHeader(2, 1, 50, 1, 0);
  packets.SetHeader(3, 1, 70, 1, 0);
  CHECK(NextPacketReadOffset(packets, 0, 4) == 3 * kBitsPerPacket + 70 + kHeaderBits);
}

REX_TEST_CASE("GetNextPacketReadOffset gives up at the header when the stream ends",
              "[kernel][xma]") {
  constexpr rex::u32 kNoFrame = 0x7FFF;
  Packets packets(4);
  packets.SetHeader(2, 1, 50, 1, 0);

  // A skip count of 0xFF marks the last packet of the stream.
  packets.SetHeader(0, 0, kNoFrame, 1, 0xFF);
  CHECK(NextPacketReadOffset(packets, 0, 4) == kHeaderBits);

  // A skip count of 0xFF is the end even where 256 packets later there is another one with a
  // frame: it is not taken as a distance.
  Packets long_stream(300);
  long_stream.SetHeader(0, 0, kNoFrame, 1, 0xFF);
  long_stream.SetHeader(256, 1, 50, 1, 0);
  CHECK(NextPacketReadOffset(long_stream, 0, 300) == kHeaderBits);

  // A skip count that leads past the buffer.
  packets.SetHeader(0, 0, kNoFrame, 1, 5);
  CHECK(NextPacketReadOffset(packets, 0, 4) == kHeaderBits);

  // Nothing to search.
  CHECK(NextPacketReadOffset(packets, 4, 4) == kHeaderBits);
}

REX_TEST_CASE("A first frame at the largest possible offset still counts as a frame start",
              "[kernel][xma]") {
  // The largest frame offset a packet can hold a frame at is kMaxFrameSizeInBits bits in; past it
  // the packet has no frame start.
  Packets packets(2);
  packets.SetHeader(0, 1, kMaxFrameSizeInBits - kHeaderBits, 1, 0);
  packets.SetHeader(1, 1, 20, 1, 0);
  CHECK(NextPacketReadOffset(packets, 0, 2) == kMaxFrameSizeInBits);

  packets.SetHeader(0, 0, kMaxFrameSizeInBits - kHeaderBits + 1, 1, 0);
  CHECK(NextPacketReadOffset(packets, 0, 2) == 1 * kBitsPerPacket + 20 + kHeaderBits);
}
