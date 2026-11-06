/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstring>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/shared_memory.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/d3d12/d3d12_util.h>

REXCVAR_DEFINE_BOOL(d3d12_tiled_shared_memory, true, "GPU/D3D12",
                    "Use tiled shared memory on D3D12")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::d3d12 {

D3D12SharedMemory::D3D12SharedMemory(D3D12CommandProcessor& command_processor,
                                     memory::Memory& memory)
    : SharedMemory(memory), command_processor_(command_processor) {}

D3D12SharedMemory::~D3D12SharedMemory() {
  Shutdown(true);
}

bool D3D12SharedMemory::Initialize() {
  InitializeCommon();

  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();

  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, kBufferSize,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  if (REXCVAR_GET(d3d12_tiled_shared_memory) &&
      provider.GetTiledResourcesTier() != D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED &&
      !provider.GetGraphicsAnalysis()) {
    if (FAILED(device->CreateReservedResource(&buffer_desc, buffer_state_, nullptr,
                                              IID_PPV_ARGS(&buffer_)))) {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB tiled buffer", kBufferSize >> 20);
      Shutdown();
      return false;
    }
    static_assert(D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES == (1 << 16));
    InitializeSparseHostGpuMemory(
        std::max(kHostGpuMemoryOptimalSparseAllocationLog2, uint32_t(16)));
  } else {
    REXGPU_INFO(
        "Direct3D 12 tiled resources are not used for shared memory "
        "emulation - video memory usage may increase significantly "
        "because a full {} MB buffer will be created",
        kBufferSize >> 20);
    if (provider.GetGraphicsAnalysis()) {
      // As of October 8th, 2018, PIX doesn't support tiled buffers.
      // FIXME(Triang3l): Re-enable tiled resources with PIX once fixed.
      REXGPU_INFO(
          "This is caused by PIX being attached, which doesn't support tiled "
          "resources yet.");
    }
    if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                               provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
                                               buffer_state_, nullptr, IID_PPV_ARGS(&buffer_)))) {
      REXGPU_ERROR("Shared memory: Failed to create the {} MB buffer", kBufferSize >> 20);
      Shutdown();
      return false;
    }
  }
  buffer_gpu_address_ = buffer_->GetGPUVirtualAddress();
  buffer_uav_writes_commit_needed_ = false;
  {
    size_t hoist_page_words = ((size_t(kBufferSize) >> page_size_log2()) + 63) / 64;
    hoist_run_active_ = false;
    hoist_pages_.assign(hoist_page_words, 0);
    hoist_pages_serial_.assign(hoist_page_words, 0);
  }

  D3D12_DESCRIPTOR_HEAP_DESC buffer_descriptor_heap_desc;
  buffer_descriptor_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  buffer_descriptor_heap_desc.NumDescriptors = uint32_t(BufferDescriptorIndex::kCount);
  buffer_descriptor_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  buffer_descriptor_heap_desc.NodeMask = 0;
  if (FAILED(device->CreateDescriptorHeap(&buffer_descriptor_heap_desc,
                                          IID_PPV_ARGS(&buffer_descriptor_heap_)))) {
    REXGPU_ERROR("Shared memory: Failed to create the descriptor heap for buffer views");
    Shutdown();
    return false;
  }
  buffer_descriptor_heap_start_ = buffer_descriptor_heap_->GetCPUDescriptorHandleForHeapStart();
  ui::d3d12::util::CreateBufferRawSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawSRV)),
      buffer_, kBufferSize);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32UintSRV)),
      buffer_, DXGI_FORMAT_R32_UINT, kBufferSize >> 2);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32UintSRV)),
      buffer_, DXGI_FORMAT_R32G32_UINT, kBufferSize >> 3);
  ui::d3d12::util::CreateBufferTypedSRV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32B32A32UintSRV)),
      buffer_, DXGI_FORMAT_R32G32B32A32_UINT, kBufferSize >> 4);
  ui::d3d12::util::CreateBufferRawUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawUAV)),
      buffer_, kBufferSize);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32UintUAV)),
      buffer_, DXGI_FORMAT_R32_UINT, kBufferSize >> 2);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32UintUAV)),
      buffer_, DXGI_FORMAT_R32G32_UINT, kBufferSize >> 3);
  ui::d3d12::util::CreateBufferTypedUAV(
      device,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kR32G32B32A32UintUAV)),
      buffer_, DXGI_FORMAT_R32G32B32A32_UINT, kBufferSize >> 4);

  upload_buffer_pool_ = std::make_unique<ui::d3d12::D3D12UploadBufferPool>(
      provider, rex::align(ui::d3d12::D3D12UploadBufferPool::kDefaultPageSize,
                           size_t(1) << page_size_log2()));

  return true;
}

void D3D12SharedMemory::Shutdown(bool from_destructor) {
  upload_buffer_pool_.reset();

  ui::d3d12::util::ReleaseAndNull(buffer_descriptor_heap_);

  // First free the buffer to detach it from the heaps.
  ui::d3d12::util::ReleaseAndNull(buffer_);

  for (ID3D12Heap* heap : buffer_tiled_heaps_) {
    heap->Release();
  }
  buffer_tiled_heaps_.clear();

  // If calling from the destructor, the SharedMemory destructor will call
  // ShutdownCommon.
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void D3D12SharedMemory::ClearCache() {
  SharedMemory::ClearCache();

  upload_buffer_pool_->ClearCache();
}

void D3D12SharedMemory::CompletedSubmissionUpdated() {
  upload_buffer_pool_->Reclaim(command_processor_.GetCompletedSubmission());
}

void D3D12SharedMemory::BeginSubmission() {
  // ExecuteCommandLists is a full UAV barrier.
  buffer_uav_writes_commit_needed_ = false;
}

void D3D12SharedMemory::CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATES new_state) {
  if (buffer_state_ == new_state) {
    if (new_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS && buffer_uav_writes_commit_needed_) {
      command_processor_.PushUAVBarrier(buffer_);
      buffer_uav_writes_commit_needed_ = false;
    }
    return;
  }
  // A run of draws (BeginDrawReads) needs the buffer to stay in the reading
  // state.
  EndHoistRun();
  command_processor_.PushTransitionBarrier(buffer_, buffer_state_, new_state);
  buffer_state_ = new_state;
  // "UAV -> anything" transition commits the writes implicitly.
  buffer_uav_writes_commit_needed_ = false;
}

void D3D12SharedMemory::WriteRawSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawSRV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_,
                                    uint32_t(BufferDescriptorIndex::kRawUAV)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteUintPow2SRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                   uint32_t element_size_bytes_pow2) {
  BufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = BufferDescriptorIndex::kR32UintSRV;
      break;
    case 3:
      descriptor_index = BufferDescriptorIndex::kR32G32UintSRV;
      break;
    case 4:
      descriptor_index = BufferDescriptorIndex::kR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_, uint32_t(descriptor_index)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void D3D12SharedMemory::WriteUintPow2UAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                   uint32_t element_size_bytes_pow2) {
  BufferDescriptorIndex descriptor_index;
  switch (element_size_bytes_pow2) {
    case 2:
      descriptor_index = BufferDescriptorIndex::kR32UintUAV;
      break;
    case 3:
      descriptor_index = BufferDescriptorIndex::kR32G32UintUAV;
      break;
    case 4:
      descriptor_index = BufferDescriptorIndex::kR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      return;
  }
  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  device->CopyDescriptorsSimple(
      1, handle,
      provider.OffsetViewDescriptor(buffer_descriptor_heap_start_, uint32_t(descriptor_index)),
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

bool D3D12SharedMemory::AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                                         uint32_t length_allocations) {
  if (!length_allocations) {
    return true;
  }

  uint32_t offset_bytes = offset_allocations << host_gpu_memory_sparse_granularity_log2();
  uint32_t length_bytes = length_allocations << host_gpu_memory_sparse_granularity_log2();

  const ui::d3d12::D3D12Provider& provider = command_processor_.GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

  D3D12_HEAP_DESC heap_desc = {};
  heap_desc.SizeInBytes = length_bytes;
  heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS | provider.GetHeapFlagCreateNotZeroed();
  ID3D12Heap* heap;
  if (FAILED(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)))) {
    REXGPU_ERROR("Shared memory: Failed to create a tile heap");
    return false;
  }
  buffer_tiled_heaps_.push_back(heap);

  D3D12_TILED_RESOURCE_COORDINATE region_start_coordinates;
  region_start_coordinates.X = offset_bytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  region_start_coordinates.Y = 0;
  region_start_coordinates.Z = 0;
  region_start_coordinates.Subresource = 0;
  D3D12_TILE_REGION_SIZE region_size;
  region_size.NumTiles = length_bytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  region_size.UseBox = FALSE;
  D3D12_TILE_RANGE_FLAGS range_flags = D3D12_TILE_RANGE_FLAG_NONE;
  UINT heap_range_start_offset = 0;
  command_processor_.AwaitSubmissionsInQueue();
  direct_queue->UpdateTileMappings(buffer_, 1, &region_start_coordinates, &region_size, heap, 1,
                                   &range_flags, &heap_range_start_offset, &region_size.NumTiles,
                                   D3D12_TILE_MAPPING_FLAG_NONE);
  command_processor_.NotifyQueueOperationsDoneDirectly();
  return true;
}

bool D3D12SharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  bool hoist = BeginHoistedUpload(upload_page_ranges);
  if (!hoist) {
    CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_DEST);
    command_processor_.SubmitBarriers();
  }
  auto& command_list = command_processor_.GetDeferredCommandList();
  bool successful = true;
  for (auto upload_range : upload_page_ranges) {
    uint32_t upload_range_start = upload_range.first;
    uint32_t upload_range_length = upload_range.second;
    while (upload_range_length != 0) {
      ID3D12Resource* upload_buffer;
      size_t upload_buffer_offset, upload_buffer_size;
      uint8_t* upload_buffer_mapping = upload_buffer_pool_->RequestPartial(
          command_processor_.GetCurrentSubmission(), upload_range_length << page_size_log2(),
          size_t(1) << page_size_log2(), &upload_buffer, &upload_buffer_offset, &upload_buffer_size,
          nullptr);
      if (upload_buffer_mapping == nullptr) {
        REXGPU_ERROR("Shared memory: Failed to get an upload buffer");
        successful = false;
        break;
      }
      MakeRangeValid(upload_range_start << page_size_log2(), uint32_t(upload_buffer_size), false);
      std::memcpy(upload_buffer_mapping,
                  memory().TranslatePhysical(upload_range_start << page_size_log2()),
                  upload_buffer_size);
      command_list.D3DCopyBufferRegion(buffer_, upload_range_start << page_size_log2(),
                                       upload_buffer, UINT64(upload_buffer_offset),
                                       UINT64(upload_buffer_size));
      uint32_t upload_buffer_pages = uint32_t(upload_buffer_size >> page_size_log2());
      upload_range_start += upload_buffer_pages;
      upload_range_length -= upload_buffer_pages;
    }
    if (!successful) {
      break;
    }
  }
  if (hoist) {
    // Also after a failure, to leave the hoisted stream well-formed.
    hoist_byte_ranges_temp_.clear();
    for (const std::pair<uint32_t, uint32_t>& range : upload_page_ranges) {
      hoist_byte_ranges_temp_.emplace_back(range.first << page_size_log2(),
                                           range.second << page_size_log2());
    }
    EndHoistedUpload(hoist_byte_ranges_temp_);
  }
  return successful;
}

// Upload hoisting (d3d12_hoist_shared_memory_uploads): the scheme (a hoist point in the deferred
// command stream, the uploads recorded in a second stream and replayed there, a bit per page for
// what the draws of the run have read) follows ForzaHorizon1-ReImagined's patch 0036
// (https://github.com/Dyotson/ForzaHorizon1-ReImagined, patches/rexglue-sdk), as ported to
// Vulkan in VulkanCommandProcessor::CanHoistSharedMemoryUpload, and the page bit code is its
// code; what a run is on Direct3D 12 (no render passes), and what ends it, is written here.
// Under its licence:
//
//   MIT License
//
//   Copyright (c) 2026 Dyotson and Forza Horizon 1: ReImagined contributors
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy
//   of this software and associated documentation files (the "Software"), to deal
//   in the Software without restriction, including without limitation the rights
//   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//   copies of the Software, and to permit persons to whom the Software is
//   furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in all
//   copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//   SOFTWARE.

bool D3D12SharedMemory::BeginHoistedUpload(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (!hoist_run_active_) {
    return false;
  }
  // The buffer has been in the reading state since the start of the run (any
  // change ends it), and no draw of the run has read these pages nor an upload
  // of the run written them.
  assert_true(buffer_state_ == kReadState);
  for (const std::pair<uint32_t, uint32_t>& range : upload_page_ranges) {
    if (!range.second) {
      continue;
    }
    uint32_t page_first = range.first;
    uint32_t page_last = range.first + range.second - 1;
    if ((page_last >> 6) >= hoist_pages_.size()) {
      return false;
    }
    for (uint32_t word = page_first >> 6; word <= (page_last >> 6); ++word) {
      if (hoist_pages_serial_[word] != hoist_serial_) {
        continue;
      }
      uint64_t mask = ~uint64_t(0);
      if (word == (page_first >> 6)) {
        mask &= ~uint64_t(0) << (page_first & 63);
      }
      if (word == (page_last >> 6)) {
        mask &= ~uint64_t(0) >> (63 - (page_last & 63));
      }
      if (hoist_pages_[word] & mask) {
        return false;
      }
    }
  }
  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();
  command_list.SetWritingHoisted(true);
  if (!hoist_run_has_uploads_) {
    hoist_run_has_uploads_ = true;
    WriteHoistedBarrier(kReadState, D3D12_RESOURCE_STATE_COPY_DEST);
  } else {
    command_list.TruncateHoistedStream(hoist_closing_barrier_offset_);
  }
  return true;
}

void D3D12SharedMemory::EndHoistedUpload(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_byte_ranges) {
  DeferredCommandList& command_list = command_processor_.GetDeferredCommandList();
  hoist_closing_barrier_offset_ = command_list.hoisted_stream_size();
  WriteHoistedBarrier(D3D12_RESOURCE_STATE_COPY_DEST, kReadState);
  command_list.SetWritingHoisted(false);
  command_list.ExtendHoistPoint(hoist_point_);
  // Another upload of these pages in the run would have to come after the first
  // one but before the draws that read them, which the hoisted copies, all at
  // the start of the run, cannot do: it is done in place.
  for (const std::pair<uint32_t, uint32_t>& range : upload_byte_ranges) {
    MarkPages(range.first, range.second);
  }
}

void D3D12SharedMemory::BeginDrawReads() {
  if (hoist_run_active_) {
    return;
  }
  if (buffer_state_ != kReadState || hoist_pages_.empty()) {
    return;
  }
  hoist_point_ = command_processor_.GetDeferredCommandList().CmdHoistPoint();
  ++hoist_serial_;
  hoist_run_has_uploads_ = false;
  hoist_run_active_ = true;
}

void D3D12SharedMemory::MarkReadByDraw(uint32_t start, uint32_t length) {
  if (!hoist_run_active_) {
    return;
  }
  MarkPages(start, length);
}

void D3D12SharedMemory::MarkPages(uint32_t start, uint32_t length) {
  if (!length || start >= kBufferSize) {
    return;
  }
  length = std::min(length, kBufferSize - start);
  uint32_t page_first = start >> page_size_log2();
  uint32_t page_last = (start + length - 1) >> page_size_log2();
  for (uint32_t word = page_first >> 6; word <= (page_last >> 6); ++word) {
    uint64_t mask = ~uint64_t(0);
    if (word == (page_first >> 6)) {
      mask &= ~uint64_t(0) << (page_first & 63);
    }
    if (word == (page_last >> 6)) {
      mask &= ~uint64_t(0) >> (63 - (page_last & 63));
    }
    if (hoist_pages_serial_[word] != hoist_serial_) {
      hoist_pages_serial_[word] = hoist_serial_;
      hoist_pages_[word] = 0;
    }
    hoist_pages_[word] |= mask;
  }
}

void D3D12SharedMemory::WriteHoistedBarrier(D3D12_RESOURCE_STATES state_before,
                                            D3D12_RESOURCE_STATES state_after) {
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = buffer_;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = state_before;
  barrier.Transition.StateAfter = state_after;
  command_processor_.GetDeferredCommandList().D3DResourceBarrier(1, &barrier);
}

}  // namespace rex::graphics::d3d12
