#include <rex/math.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_WIN32, "This file is Windows-only");

#include <intrin.h>

namespace rex {

// lzcnt, tzcnt and bit_scan_forward are defined inline in rex/math.h, where
// the GPU command thread's hot path can inline them.

}  // namespace rex
