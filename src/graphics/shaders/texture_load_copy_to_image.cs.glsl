#version 460

// Vulkan texture loading, last step: copies one mip level of a texture from
// the linear layout the texture load shaders write to the scratch buffer into
// the image, through an unsigned integer storage view of the same texel size,
// instead of vkCmdCopyBufferToImage (which runs at a fraction of the memory
// bandwidth on some drivers - vulkan_texture_load_storage_copy).
// XE_COPY_TEXEL_DWORDS: 1 (32bpp, r32ui view), 2 (64bpp, rgba16ui view - a core
// storage format, unlike rg32ui) or 4 (128bpp, rgba32ui view).

#ifndef XE_COPY_TEXEL_DWORDS
#define XE_COPY_TEXEL_DWORDS 1
#endif

layout(local_size_x = 32, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeCopyToImageConstants {
  // In texels, from the start of the bound part of the buffer.
  uint xe_copy_offset;
  uint xe_copy_row_pitch;
  uint xe_copy_layer_pitch;
  uint xe_copy_width;
  uint xe_copy_height;
};

#if XE_COPY_TEXEL_DWORDS == 1
layout(set = 0, binding = 0, std430) readonly buffer XeCopySource {
  uint xe_copy_source[];
};
layout(set = 1, binding = 0, r32ui) uniform writeonly uimage2DArray xe_copy_dest;
#elif XE_COPY_TEXEL_DWORDS == 2
layout(set = 0, binding = 0, std430) readonly buffer XeCopySource {
  uvec2 xe_copy_source[];
};
layout(set = 1, binding = 0, rgba16ui) uniform writeonly uimage2DArray xe_copy_dest;
#else
layout(set = 0, binding = 0, std430) readonly buffer XeCopySource {
  uvec4 xe_copy_source[];
};
layout(set = 1, binding = 0, rgba32ui) uniform writeonly uimage2DArray xe_copy_dest;
#endif

void main() {
  uvec3 texel = gl_GlobalInvocationID;
  if (texel.x >= xe_copy_width || texel.y >= xe_copy_height) {
    return;
  }
  uint index = xe_copy_offset + texel.z * xe_copy_layer_pitch + texel.y * xe_copy_row_pitch +
               texel.x;
#if XE_COPY_TEXEL_DWORDS == 1
  uvec4 value = uvec4(xe_copy_source[index], 0u, 0u, 0u);
#elif XE_COPY_TEXEL_DWORDS == 2
  // Little-endian: the low half of each dword is the lower-numbered component.
  uvec2 dwords = xe_copy_source[index];
  uvec4 value = uvec4(dwords.x & 0xFFFFu, dwords.x >> 16u, dwords.y & 0xFFFFu, dwords.y >> 16u);
#else
  uvec4 value = xe_copy_source[index];
#endif
  imageStore(xe_copy_dest, ivec3(texel), value);
}
