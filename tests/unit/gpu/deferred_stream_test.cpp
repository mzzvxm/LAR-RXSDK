/**
 * @file        tests/unit/gpu/deferred_stream_test.cpp
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "rex_test.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "gpu/xenos/host/deferred_stream.h"

namespace {

enum class TestCommand : rex::u32 {
  kNothing,
  kOne,
  kTwo,
};

using Stream = rex::gpu::DeferredStream<TestCommand>;

// Writes a command with one 64-bit argument and returns where the command starts, in elements.
size_t WriteOne(Stream& stream, TestCommand command, rex::u64 argument) {
  const size_t start = stream.Size();
  void* arguments = stream.Write(command, sizeof(argument));
  std::memcpy(arguments, &argument, sizeof(argument));
  return start;
}

rex::u64 ArgumentAt(const uintmax_t* elements, size_t command_start) {
  rex::u64 argument;
  std::memcpy(&argument, elements + command_start + Stream::kHeaderSizeElements, sizeof(argument));
  return argument;
}

}  // namespace

REX_TEST_CASE("DeferredStream::Swap hands the commands over and takes an empty vector back",
              "[gpu][deferred]") {
  Stream stream;
  const size_t first = WriteOne(stream, TestCommand::kOne, 0x1111);
  const size_t second = WriteOne(stream, TestCommand::kTwo, 0x2222);
  const size_t size = stream.Size();
  REQUIRE(size == 2 * (Stream::kHeaderSizeElements + 1));

  // The vector coming in keeps its allocation for the next recording and must be empty on return.
  std::vector<uintmax_t> handed;
  handed.reserve(64);
  stream.Swap(handed);
  CHECK(stream.Size() == 0);
  REQUIRE(handed.size() == size);
  CHECK(ArgumentAt(handed.data(), first) == 0x1111);
  CHECK(ArgumentAt(handed.data(), second) == 0x2222);
  CHECK(reinterpret_cast<const Stream::Header*>(handed.data() + first)->command ==
        TestCommand::kOne);
  CHECK(reinterpret_cast<const Stream::Header*>(handed.data() + second)->command ==
        TestCommand::kTwo);

  // Recording goes on into the vector that came back, and the commands handed over stay as they
  // were.
  WriteOne(stream, TestCommand::kOne, 0x3333);
  CHECK(stream.Size() == Stream::kHeaderSizeElements + 1);
  CHECK(ArgumentAt(handed.data(), first) == 0x1111);
}

REX_TEST_CASE("DeferredStream::Truncate drops the commands after a size", "[gpu][deferred]") {
  Stream stream;
  WriteOne(stream, TestCommand::kOne, 1);
  const size_t keep = stream.Size();
  WriteOne(stream, TestCommand::kTwo, 2);
  WriteOne(stream, TestCommand::kTwo, 3);
  stream.Truncate(keep);
  CHECK(stream.Size() == keep);
  // The next command starts where the dropped ones did.
  const size_t start = WriteOne(stream, TestCommand::kOne, 4);
  CHECK(start == keep);
  CHECK(ArgumentAt(stream.Data(), start) == 4);
  CHECK(ArgumentAt(stream.Data(), 0) == 1);

  stream.Truncate(0);
  CHECK(stream.Size() == 0);
}

REX_TEST_CASE("DeferredStream::Data changes the arguments of a command written earlier",
              "[gpu][deferred]") {
  Stream stream;
  const size_t first = WriteOne(stream, TestCommand::kOne, 5);
  // More commands may move the storage; the offset stays good.
  for (rex::u64 i = 0; i < 1000; ++i) {
    WriteOne(stream, TestCommand::kTwo, i);
  }
  const rex::u64 changed = 6;
  std::memcpy(stream.Data() + first + Stream::kHeaderSizeElements, &changed, sizeof(changed));
  CHECK(ArgumentAt(stream.Data(), first) == 6);
  CHECK(ArgumentAt(stream.Data(), first + 2 * (Stream::kHeaderSizeElements + 1)) == 1);
}
