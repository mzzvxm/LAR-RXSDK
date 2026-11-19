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

#include "gpu/xenos/shared_memory.h"
#include <rex/memory.h>
#include "gpu/xenos/host/vulkan/vulkan_upload_buffer_pool.h"

namespace rex::gpu::vulkan {

class VulkanCommandProcessor;

class VulkanSharedMemory : public SharedMemory {
 public:
  VulkanSharedMemory(VulkanCommandProcessor& command_processor, IPhysicalMemory& memory,
                     IPhysicalWriteWatch& watch, VkPipelineStageFlags guest_shader_pipeline_stages);
  ~VulkanSharedMemory() override;

  bool Initialize();
  void Shutdown(bool from_destructor = false);

  void CompletedSubmissionUpdated();
  void EndSubmission();

  enum class Usage : u8 {
    /** Index buffer, vfetch, compute read, transfer source. */
    kRead,
    /** Index buffer, vfetch, memexport. */
    kGuestDrawReadWrite,
    kComputeWrite,
    kTransferDestination,
  };
  /**
   * Inserts the barrier for a usage change.
   *
   * @param written_range Byte offset and length the usage writes.
   */
  void Use(Usage usage, std::pair<u32, u32> written_range = {});

  VkBuffer GetBuffer() const { return buffer_; }

  using SharedMemory::GetPageSizeLog2;

 protected:
  bool AllocateSparseHostGPUMemoryRange(u32 offset_allocations, u32 length_allocations) override;

  bool UploadRanges(const std::vector<std::pair<u32, u32>>& upload_page_ranges) override;
  bool UploadByteRanges(const std::vector<std::pair<u32, u32>>& upload_byte_ranges) override;

 private:
  void GetUsageMasks(Usage usage, VkPipelineStageFlags& stage_mask,
                     VkAccessFlags& access_mask) const;
  // Upload hoisting: whether an upload of these pages can go before the
  // current render pass, and if so, starts recording it there with the
  // barrier before the hoisted copies.
  bool BeginHoistedUpload(const std::vector<std::pair<u32, u32>>& upload_page_ranges);
  // Ends recording a hoisted upload of these byte ranges with the barrier after
  // the hoisted copies.
  void EndHoistedUpload(const std::vector<std::pair<u32, u32>>& upload_byte_ranges);
  // A barrier on the whole buffer, written into the deferred command buffer
  // directly.
  void WriteUsageBarrier(Usage from, Usage to);

  VulkanCommandProcessor& command_processor_;
  VkPipelineStageFlags guest_shader_pipeline_stages_;

  VkBuffer buffer_ = VK_NULL_HANDLE;
  u32 buffer_memory_type_;
  /** One allocation, or every allocation so far when sparse. */
  std::vector<VkDeviceMemory> buffer_memory_;

  Usage last_usage_;
  std::pair<u32, u32> last_written_range_;

  // Uploads hoisted before a render pass
  // (VulkanCommandProcessor::CanHoistSharedMemoryUpload) share one barrier
  // before their copies and one after: the render pass they were hoisted
  // before, and where its closing barrier is in the hoisted command stream
  // (dropped and written again after the next upload's copies).
  u64 hoist_render_pass_serial_ = 0;
  size_t hoist_closing_barrier_offset_ = 0;
  std::vector<std::pair<u32, u32>> hoist_page_ranges_temp_;
  // The pages (first, count) that the byte ranges of UploadByteRanges are in.
  std::vector<std::pair<u32, u32>> hoist_byte_pages_temp_;

  std::unique_ptr<gpu::vulkan::VulkanUploadBufferPool> upload_buffer_pool_;
  std::vector<VkBufferCopy> upload_regions_;
};

}  // namespace rex::gpu::vulkan
