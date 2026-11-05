/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#version 460

// Compute shader to downscale scaled resolve buffer data back to 1x resolution.
// Operates on 32x32 tiled data format used by Xbox 360.
// Each thread handles one output pixel (one 32x32 tile = 1024 threads).
//
// By default, picks the top-left pixel of each scale_x * scale_y block.
// When xe_downscale_half_pixel_offset is set, samples from (scale/2, scale/2)
// within each block to compensate for the half-pixel offset becoming a
// full-pixel offset at higher resolutions.
//
// Where the pixel is in the scaled data depends on the layout of a resolution-
// scaled resolve (XeTextureScaledTiledOffset in the xenia shaders): the host
// pixels of a guest Nx1 unit (N = 8 for 1 and 2 bytes a pixel, 4 for 4 bytes,
// 2 for 8 bytes) are kept in N x 1 host sub-units, column-major, not as one
// contiguous block per guest pixel. With xe_downscale_unit_layout set the
// source is found that way (XeDownscaleSourceOffset, which the layout check in
// launcher/dev runs as text); without it, the pixel is taken to be at
// pixel * scale_x * scale_y in the tile's scaled data, which is the block's
// top-left host pixel only at 1x and for 8 bytes a pixel at 2x2.

layout(local_size_x = 32, local_size_y = 32, local_size_z = 1) in;

layout(push_constant) uniform ResolveDownscaleConstants {
  uint xe_downscale_scale_x;           // 1 to kMaxDrawResolutionScaleAlongAxis
  uint xe_downscale_scale_y;           // 1 to kMaxDrawResolutionScaleAlongAxis
  uint xe_downscale_pixel_size_log2;   // 0=8bit, 1=16bit, 2=32bit, 3=64bit
  uint xe_downscale_tile_count;        // Number of 32x32 tiles to process
  uint xe_downscale_source_offset_bytes;   // Byte offset into source buffer
  // When non-zero, apply half-pixel offset correction by sampling from
  // (scale/2, scale/2) within each scaled block instead of (0, 0).
  // This compensates for the D3D9-style half-pixel offset used by Xbox 360
  // games, which at Nx resolution scaling shifts rendered content by
  // (N/2, N/2) host pixels.
  uint xe_downscale_half_pixel_offset;
  // When non-zero, find the pixel by the scaled resolve layout (see above).
  uint xe_downscale_unit_layout;
};

// Source buffer (scaled resolve data)
layout(std430, set = 0, binding = 0) readonly buffer SourceBuffer {
  uint data[];
} xe_resolve_source;

// Destination buffer (1x resolution)
layout(std430, set = 0, binding = 1) writeonly buffer DestBuffer {
  uint data[];
} xe_resolve_dest;

// BEGIN XeDownscaleSourceOffset (the same text is in resolve_downscale.cs.hlsl
// and is what launcher/dev/downscale-layout-check.py runs)
// The byte offset, from the start of the scaled data of the first tile, of the
// host pixel to copy for the guest pixel at index pixel_index (the order of the
// pixels in memory) of 32x32 tile tile_index.
uint XeDownscaleSourceOffset(uint tile_index, uint pixel_index, uint scale_x,
                             uint scale_y, uint pixel_size_log2,
                             uint half_pixel_offset, uint unit_layout) {
  uint pixel_size = 1u << pixel_size_log2;
  uint tile_size_1x = 32u * 32u * pixel_size;
  uint scale_xy = scale_x * scale_y;
  // Where in the block of scale_x * scale_y host pixels to sample: the top left
  // one, or, with the half-pixel correction, the one at (scale/2, scale/2) to
  // compensate for the D3D9-style half-pixel offset shifting content by
  // (N/2, N/2) pixels at Nx resolution.
  uint offset_x = 0u;
  uint offset_y = 0u;
  if (half_pixel_offset != 0u && scale_xy > 1u) {
    offset_x = scale_x >> 1u;
    offset_y = scale_y >> 1u;
  }
  uint src_offset;
  if (unit_layout != 0u) {
    // Guest units of N pixels consecutive in memory (XeTextureTiled
    // ConsecutiveBlocksLog2), each scaled to scale_x * scale_y sub-units of N
    // host pixels in a row, column-major.
    uint unit_width_log2 = min(4u - pixel_size_log2, 3u);
    uint unit_mask = (1u << unit_width_log2) - 1u;
    uint unit_pixel = pixel_index & unit_mask;
    // The guest unit's byte offset in the 1x data, which the scaled data's unit
    // is at scale_x * scale_y times.
    uint unit_offset_1x = tile_index * tile_size_1x +
                          ((pixel_index & ~unit_mask) << pixel_size_log2);
    // The host pixel to take, in host pixels along the unit's rows.
    uint host_x = unit_pixel * scale_x + offset_x;
    uint sub_unit_x = host_x >> unit_width_log2;
    uint host_pixel_in_sub_unit = host_x & unit_mask;
    src_offset = unit_offset_1x * scale_xy +
                 ((((sub_unit_x * scale_y + offset_y) << unit_width_log2) +
                   host_pixel_in_sub_unit)
                  << pixel_size_log2);
  } else {
    // The pixel's slot, in pixels, among the tile's scaled pixels.
    uint block_sample_offset = offset_x + offset_y * scale_x;
    src_offset = tile_index * tile_size_1x * scale_xy +
                 pixel_index * pixel_size * scale_xy +
                 block_sample_offset * pixel_size;
  }
  return src_offset;
}
// END XeDownscaleSourceOffset

// Groupshared memory for coalescing sub-32-bit writes
shared uint gs_tile_data[32 * 32];

void main() {
  uint tile_index = gl_WorkGroupID.x;

  // Early out if beyond tile count
  if (tile_index >= xe_downscale_tile_count) {
    return;
  }

  uint row = gl_LocalInvocationID.y;
  uint column = gl_LocalInvocationID.x;
  uint pixel_index = row * 32u + column;  // 0-1023

  uint pixel_size = 1u << xe_downscale_pixel_size_log2;
  uint tile_size_1x = 32u * 32u * pixel_size;

  // Source offset in bytes: the pixel's slot in the scaled data, plus the
  // offset of that data in the source buffer (added before the division to
  // keep the sub-dword byte position).
  uint src_byte_offset =
      xe_downscale_source_offset_bytes +
      XeDownscaleSourceOffset(tile_index, pixel_index, xe_downscale_scale_x,
                              xe_downscale_scale_y,
                              xe_downscale_pixel_size_log2,
                              xe_downscale_half_pixel_offset,
                              xe_downscale_unit_layout);
  uint src_offset = src_byte_offset >> 2u;

  // Destination offset in 1x buffer
  uint dst_byte_offset = tile_index * tile_size_1x +
                         pixel_index * pixel_size;
  uint dst_offset = dst_byte_offset >> 2u;

  // Copy pixel based on size
  switch (xe_downscale_pixel_size_log2) {
    case 0u: {  // 8-bit - use groupshared to coalesce 4 bytes into 32-bit writes
      // Load the byte value
      uint src_word = xe_resolve_source.data[src_offset];
      uint byte_val = (src_word >> ((src_byte_offset & 3u) * 8u)) & 0xFFu;

      // Each group of 4 threads packs into one uint
      uint pack_index = pixel_index >> 2u;   // Which uint (0-255)
      uint byte_pos = pixel_index & 3u;      // Which byte in uint (0-3)

      // Pack byte into shared memory
      uint contribution = byte_val << (byte_pos * 8u);
      if (byte_pos == 0u) {
        gs_tile_data[pack_index] = contribution;
      }
      barrier();
      if (byte_pos != 0u) {
        atomicOr(gs_tile_data[pack_index], contribution);
      }
      barrier();

      // First thread of each 4 writes the packed uint
      if (byte_pos == 0u) {
        xe_resolve_dest.data[tile_index * (32u * 32u / 4u) + pack_index] =
            gs_tile_data[pack_index];
      }
      break;
    }
    case 1u: {  // 16-bit - use groupshared to coalesce 2 shorts into 32-bit writes
      // Load the short value
      uint src_word = xe_resolve_source.data[src_offset];
      uint short_val = (src_word >> ((src_byte_offset & 2u) * 8u)) & 0xFFFFu;

      // Each group of 2 threads packs into one uint
      uint pack_index = pixel_index >> 1u;   // Which uint (0-511)
      uint short_pos = pixel_index & 1u;     // Which short in uint (0-1)

      // Pack short into shared memory
      uint contribution = short_val << (short_pos * 16u);
      if (short_pos == 0u) {
        gs_tile_data[pack_index] = contribution;
      }
      barrier();
      if (short_pos != 0u) {
        atomicOr(gs_tile_data[pack_index], contribution);
      }
      barrier();

      // First thread of each 2 writes the packed uint
      if (short_pos == 0u) {
        xe_resolve_dest.data[tile_index * (32u * 32u * 2u / 4u) + pack_index] =
            gs_tile_data[pack_index];
      }
      break;
    }
    case 2u: {  // 32-bit - direct copy
      xe_resolve_dest.data[dst_offset] = xe_resolve_source.data[src_offset];
      break;
    }
    case 3u: {  // 64-bit - direct copy (2 uints)
      xe_resolve_dest.data[dst_offset] = xe_resolve_source.data[src_offset];
      xe_resolve_dest.data[dst_offset + 1u] =
          xe_resolve_source.data[src_offset + 1u];
      break;
    }
  }
}
