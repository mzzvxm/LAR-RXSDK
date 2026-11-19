/**
 * @file        gpu/xenos_graphics_driver.h
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/gpu/graphics_driver.h>
#include <rex/types.h>

namespace rex::gpu {

/**
 * A Xenos GPU. It adds what the guest kernel needs beyond presenting. GraphicsSystem routes the
 * kernel's calls here only when the title requires the Xenos family.
 */
class IXenosGraphicsDriver : public IGraphicsDriver {
 public:
  IXenosGraphicsDriver* AsXenos() final { return this; }

  /**
   * Starts reading commands from the guest ring buffer. Guest thread.
   *
   * @param size_log2 Log2 of its size in quadwords.
   */
  virtual void InitializeRingBuffer(u32 address, u32 size_log2) = 0;

  /**
   * Writes the ring's read pointer to guest memory as commands complete. Guest thread.
   *
   * @param address Guest physical address to write it to.
   * @param block_size_log2 Log2 of the quadwords between writes.
   */
  virtual void EnableReadPointerWriteBack(u32 address, u32 block_size_log2) = 0;

  /** Reads a GPU register for the guest. Any guest thread. */
  virtual u32 ReadRegister(u32 index) = 0;

  /** Writes a GPU register for the guest. Any guest thread. */
  virtual void WriteRegister(u32 index, u32 value) = 0;

  /**
   * Asks for the resolves that write a range of guest physical memory to be read back to it
   * synchronously for a number of frames, whatever the driver's readback settings say, so the CPU
   * sees what the GPU rendered there. A driver without a command processor takes no request. Any
   * thread.
   *
   * @param physical_address Guest physical address of the range.
   * @param length Bytes in the range.
   * @param frames How many swaps the request lasts.
   * @return True when the driver took the request.
   */
  virtual bool RequestResolveReadback(u32 physical_address, u32 length, u32 frames) {
    (void)physical_address;
    (void)length;
    (void)frames;
    return false;
  }
};

}  // namespace rex::gpu
