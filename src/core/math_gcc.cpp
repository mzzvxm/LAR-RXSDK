#include <rex/math.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
static_assert(false, "This file is non-MSVC only");
#endif

namespace rex {

// lzcnt, tzcnt and bit_scan_forward are defined inline in rex/math.h, where
// the GPU command thread's hot path can inline them.

}  // namespace rex
