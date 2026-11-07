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

#pragma once

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include <rex/graphics/shared_memory.h>
#include <rex/memory.h>
#include <rex/ui/d3d12/d3d12_api.h>
#include <rex/ui/d3d12/d3d12_upload_buffer_pool.h>

namespace rex::graphics::d3d12 {

class D3D12CommandProcessor;

class D3D12SharedMemory : public SharedMemory {
 public:
  D3D12SharedMemory(D3D12CommandProcessor& command_processor, memory::Memory& memory);
  ~D3D12SharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);
  void ClearCache() override;

  ID3D12Resource* GetBuffer() const { return buffer_; }
  D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress() const { return buffer_gpu_address_; }

  void CompletedSubmissionUpdated();
  void BeginSubmission();

  // RequestRange may transition the buffer to copy destination - call it before
  // UseForReading or UseForWriting.

  // Makes the buffer usable for vertices, indices and texture untiling. Ends
  // the run of draws that uploads can be hoisted to the start of (below): the
  // texture loads that call it read the buffer without saying which pages.
  void UseForReading() {
    EndHoistRun();
    UseForReadingInDraw();
  }
  // The same for a draw, which tells which pages it reads with MarkReadByDraw
  // after the barriers (BeginDrawReads), and so does not end the run.
  void UseForReadingInDraw() {
    // Vertex fetch is also allowed in pixel shaders.
    CommitUAVWritesAndTransitionBuffer(kReadState);
  }
  // Makes the buffer usable for texture tiling after a resolve.
  void UseForWriting() {
    EndHoistRun();
    CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  // Makes the buffer usable as a source for copy commands.
  void UseAsCopySource() {
    EndHoistRun();
    CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATE_COPY_SOURCE);
  }
  // Must be called when doing draws/dispatches modifying data within the shared
  // memory buffer as a UAV, to make sure that when UseForWriting is called the
  // next time, a UAV barrier will be done, and subsequent overlapping UAV
  // writes and reads are ordered.
  void MarkUAVWritesCommitNeeded() {
    if (buffer_state_ == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
      buffer_uav_writes_commit_needed_ = true;
    }
  }

  // Upload hoisting (d3d12_hoist_shared_memory_uploads). Every upload into the
  // buffer used to copy between two draws, with the buffer going from the
  // reading state to the copy destination state and back, which makes the GPU
  // wait for the draws before it to finish and the draws after it to wait for
  // the copy. A "run" starts at a draw with the buffer in the reading state and
  // goes on while it stays so and only draws use it. An upload of pages that no
  // draw of the run has read yet is recorded at the start of the run instead
  // (the command list's hoisted commands), between one pair of barriers that
  // all the uploads of the run share. The same bytes reach the same draws:
  // the draws before the upload do not read these pages, and nothing else
  // reads or writes the buffer in the run.
  //
  // Called by a draw that reads the buffer, after UseForReadingInDraw and the
  // barriers have been submitted (the buffer is in the reading state in the
  // command list now): starts a run if there is none.
  void BeginDrawReads();
  // The draw reads these bytes of the buffer (vertex fetches, guest indices).
  void MarkReadByDraw(uint32_t start, uint32_t length);
  // Anything else that uses the buffer, and the end of the command list, end the
  // run: uploads after it are done in place.
  // CHECK: the exactness is argued, not proved (the pictures of the fork's check: the pause
  // menu identical, free roam one view marginally over its limit). Every other user of the
  // buffer in this tree goes through UseForReading, UseForWriting or UseAsCopySource (the texture
  // loads, direct resolves, scaled resolve copies, memexport readback, index copies), read from
  // the code; a later one has to call EndHoistRun.
  void EndHoistRun() { hoist_run_active_ = false; }

  void WriteRawSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle);
  void WriteRawUAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle);
  // Due to the D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP limitation, the
  // smallest supported formats are 32-bit.
  void WriteUintPow2SRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                  uint32_t element_size_bytes_pow2);
  void WriteUintPow2UAVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                  uint32_t element_size_bytes_pow2);

 protected:
  bool AllocateSparseHostGpuMemoryRange(uint32_t offset_allocations,
                                        uint32_t length_allocations) override;

  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  D3D12CommandProcessor& command_processor_;

  // The 512 MB tiled buffer.
  ID3D12Resource* buffer_ = nullptr;
  D3D12_GPU_VIRTUAL_ADDRESS buffer_gpu_address_ = 0;
  std::vector<ID3D12Heap*> buffer_tiled_heaps_;
  D3D12_RESOURCE_STATES buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  bool buffer_uav_writes_commit_needed_ = false;
  void CommitUAVWritesAndTransitionBuffer(D3D12_RESOURCE_STATES new_state);

  // The state for vertices, indices and texture untiling.
  static constexpr D3D12_RESOURCE_STATES kReadState = D3D12_RESOURCE_STATES(
      int(D3D12_RESOURCE_STATE_INDEX_BUFFER) | int(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) |
      int(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

  // Upload hoisting. Whether a run is going on: the buffer has been in the
  // reading state since the hoist point (in the command list's main stream, at
  // the draw that started the run, after the barriers before it) and nothing but
  // draws has used it.
  bool hoist_run_active_ = false;
  size_t hoist_point_ = 0;
  // Whether the hoisted stream has the barrier to the copy destination state
  // for this run already, and where the barrier back is (dropped and written
  // again after the next upload's copies).
  bool hoist_run_has_uploads_ = false;
  size_t hoist_closing_barrier_offset_ = 0;
  // The pages the draws of the run read and the uploads of the run wrote, a bit
  // per page, in 64-page words that only count if their serial is the run's, so
  // nothing needs clearing between runs.
  uint64_t hoist_serial_ = 0;
  std::vector<uint64_t> hoist_pages_;
  std::vector<uint64_t> hoist_pages_serial_;
  std::vector<std::pair<uint32_t, uint32_t>> hoist_byte_ranges_temp_;
  // Whether an upload of these page ranges (first page, page count) can be
  // hoisted now; if so, starts recording it in the hoisted stream.
  bool BeginHoistedUpload(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges);
  // Ends recording a hoisted upload of these byte ranges.
  void EndHoistedUpload(const std::vector<std::pair<uint32_t, uint32_t>>& upload_byte_ranges);
  void MarkPages(uint32_t start, uint32_t length);
  void WriteHoistedBarrier(D3D12_RESOURCE_STATES state_before, D3D12_RESOURCE_STATES state_after);

  // Non-shader-visible buffer descriptor heap for faster binding (via copying
  // rather than creation).
  enum class BufferDescriptorIndex : uint32_t {
    kRawSRV,
    kR32UintSRV,
    kR32G32UintSRV,
    kR32G32B32A32UintSRV,
    kRawUAV,
    kR32UintUAV,
    kR32G32UintUAV,
    kR32G32B32A32UintUAV,

    kCount,
  };
  ID3D12DescriptorHeap* buffer_descriptor_heap_ = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE buffer_descriptor_heap_start_;

  std::unique_ptr<ui::d3d12::D3D12UploadBufferPool> upload_buffer_pool_;
};

}  // namespace rex::graphics::d3d12
