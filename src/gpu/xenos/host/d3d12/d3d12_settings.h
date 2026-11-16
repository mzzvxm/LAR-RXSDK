/**
 * @file        gpu/xenos/host/d3d12/d3d12_settings.h
 * @brief       Settings of the Direct3D 12 driver
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <string>

#include <rex/setting.h>
#include <rex/types.h>

namespace rex::gpu::d3d12 {

/** The gpu.d3d12 settings. The D3D12 graphics driver owns one. */
class D3D12Settings : public SettingOwner {
 public:
  D3D12Settings() : SettingOwner("gpu.d3d12") {}

  REX_SETTING(i32, adapter, -1, "Adapter index, -1 picks the first hardware adapter, -2 picks WARP",
              Range{-2, 15}, kInitOnly);
  REX_SETTING(bool, debug_markers, false,
              "Insert debug markers into GPU command streams for tools like PIX and RenderDoc. "
              "Turns on when RenderDoc is detected");
  REX_SETTING(bool, bindless, true, "Use bindless resources where available", kRequiresRestart);
  REX_SETTING(bool, readback_memexport, false,
              "Read data written by memory export in shaders on the CPU");
  REX_SETTING(bool, readback_resolve, false, "Read render to texture results on the CPU");
  REX_SETTING(bool, submit_on_primary_buffer_end, true,
              "Submit command list when PM4 primary buffer ends");
  REX_SETTING(bool, threaded_submission, true,
              "Replay the recorded Direct3D 12 command lists into the real command list and submit "
              "them on a thread of their own instead of the GPU command processor's thread. Off: "
              "the command processor thread does it at the end of every submission",
              kDebugOnly);
  REX_SETTING(u32, threaded_submission_split_draws, 1000,
              "With threaded_submission, end a submission after this many draws (0: only where the "
              "guest's command buffers end), so the submission thread replays it while the next "
              "draws are recorded and less is left to replay when a frame is presented",
              Range{0, 100000}, kDebugOnly);
  REX_SETTING(bool, hoist_shared_memory_uploads, false,
              "Record the shared memory uploads (vertex and index data the CPU rewrote, textures) "
              "at the start of the run of draws they are for, between one pair of barriers for the "
              "run, when no draw before the upload has read the uploaded pages, instead of copying "
              "between the draws",
              kDebugOnly);
  REX_SETTING(bool, dxbc_disasm, false, "Dump DXBC disassembly");
  REX_SETTING(bool, dxbc_disasm_dxilconv, false, "Dump DXIL conversion disassembly");
  REX_SETTING(i32, pipeline_creation_threads, -1,
              "Number of pipeline creation threads, -1 for auto", Range{-1, 32}, kRequiresRestart);
  REX_SETTING(u32, async_skip_incomplete_frames_max_ms, 100,
              "With async shader compilation, frames with draws left out because their pipelines "
              "were still being created are not presented (the previous picture stays), as on "
              "Vulkan. Once presents have been skipped for this long, such frames are presented "
              "anyway, so a slow pipeline creation shows as objects missing for a moment rather "
              "than a picture held still. 0 skips for as long as it takes");
  REX_SETTING(bool, tessellation_wireframe, false, "Render tessellation as wireframe");
  REX_SETTING(bool, native_stencil_value_output, true, "Enable native stencil value output");
  REX_SETTING(bool, use_fuzzy_alpha_epsilon, false,
              "Use approximate compare for alpha test values to prevent flickering on NVIDIA "
              "graphics cards");
  REX_SETTING(bool, dxbc_switch, true, "Use switch statements in DXBC");
  REX_SETTING(bool, dxbc_source_map, false, "Generate source maps for DXBC");
  REX_SETTING(bool, draw_resolution_scaled_texture_offsets, true,
              "Scale texture offsets with draw resolution");
  REX_SETTING(bool, native_stencil_value_output_intel, false,
              "Native stencil value output for Intel");
  REX_SETTING(std::string, render_target_path, "", "Render target implementation path",
              kInitOnly);
  REX_SETTING(bool, tiled_shared_memory, true, "Use tiled shared memory", kRequiresRestart);
  REX_SETTING(bool, texture_heaps, true,
              "Place the textures that are not render targets or depth buffers in shared 256 MB "
              "heaps instead of making each one a committed resource of its own. Creating a "
              "committed resource costs the GPU command thread about 0.19 ms and releasing one "
              "0.08 ms (RTX 5080), so streaming in or evicting hundreds of textures in a frame "
              "took over 100 ms; a placed texture takes about 0.01 ms. Off: committed resources "
              "only, to compare",
              kDebugOnly);
  REX_SETTING(bool, allow_tearing, true, "Allow variable refresh rate and tearing");
  REX_SETTING(bool, debug, false, "Enable Direct3D 12 and DXGI debug layer", kInitOnly);
  REX_SETTING(bool, break_on_error, false, "Break on Direct3D 12 validation errors");
  REX_SETTING(bool, break_on_warning, false, "Break on Direct3D 12 validation warnings");
  REX_SETTING(i32, queue_priority, 1,
              "Graphics command queue priority, 0 for normal, 1 for high, 2 for realtime",
              Range{0, 2});
};

}  // namespace rex::gpu::d3d12
