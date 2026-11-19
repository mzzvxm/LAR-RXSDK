"""Validates the compiled resolve shaders the SDK carries (the fast resolves and the downscale).

Usage: python scripts/validate_resolve_shaders.py [--spirv-val <path>]

The .xesl and HLSL sources are not in every tree, so the compiled arrays are what can be checked:

  src/gpu/xenos/shaders/vulkan_spirv/resolve_*_cs.h       SPIR-V, run through spirv-val
  src/gpu/xenos/shaders/bytecode/d3d12_5_1/resolve_*_cs.h  DXBC (shader model 5.1), whose container
                                                          is checked and which the Direct3D
                                                          compiler library must be able to
                                                          disassemble (Windows only)

dxv validates DXIL, and these are DXBC, so it is not used. spirv-val is --spirv-val, else the first
of spirv-val on the PATH and the Vulkan SDK's Bin folder. The exit code is 1 when an array fails,
2 when a tool is missing, 0 otherwise.
"""

import argparse
import ctypes
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPIRV_DIR = ROOT / "src/gpu/xenos/shaders/vulkan_spirv"
DXBC_DIR = ROOT / "src/gpu/xenos/shaders/bytecode/d3d12_5_1"
ARRAY = re.compile(r"\[\]\s*=\s*\{(.*?)\};", re.DOTALL)


def parse_array(path: Path) -> list[int]:
    match = ARRAY.search(path.read_text(encoding="utf-8"))
    if not match:
        raise ValueError(f"{path.name}: no array")
    return [int(token, 0) for token in re.findall(r"0[xX][0-9a-fA-F]+|\d+", match.group(1))]


def find_spirv_val(explicit: str | None) -> str | None:
    candidates = [explicit] if explicit else []
    candidates.append(shutil.which("spirv-val"))
    candidates += [str(p) for p in sorted(Path("C:/VulkanSDK").glob("*/Bin/spirv-val.exe"))]
    return next((c for c in candidates if c and Path(c).exists()), None)


def check_spirv(path: Path, spirv_val: str, work: Path) -> list[str]:
    words = parse_array(path)
    if not words or words[0] != 0x07230203:
        return ["the SPIR-V magic number is missing"]
    blob = work / (path.stem + ".spv")
    blob.write_bytes(struct.pack(f"<{len(words)}I", *words))
    result = subprocess.run([spirv_val, "--target-env", "vulkan1.1", str(blob)],
                            capture_output=True, text=True)
    return [] if result.returncode == 0 else [(result.stdout + result.stderr).strip()]


def check_dxbc(path: Path) -> list[str]:
    data = bytes(parse_array(path))
    problems = []
    if data[:4] != b"DXBC":
        return ["the DXBC magic is missing"]
    total_size, chunk_count = struct.unpack_from("<II", data, 24)
    if total_size != len(data):
        problems.append(f"the container says {total_size} bytes, the array has {len(data)}")
    for index in range(chunk_count):
        (offset,) = struct.unpack_from("<I", data, 32 + 4 * index)
        if offset + 8 > len(data):
            problems.append(f"chunk {index} starts at {offset}, past the end")
            continue
        _, size = struct.unpack_from("<4sI", data, offset)
        if offset + 8 + size > len(data):
            problems.append(f"chunk {index} ({data[offset:offset + 4]!r}) runs past the end")
    if sys.platform == "win32":
        problems += disassemble(data)
    return problems


def disassemble(data: bytes) -> list[str]:
    try:
        compiler = ctypes.WinDLL("d3dcompiler_47.dll")
    except OSError:
        return []
    blob = ctypes.c_void_p()
    compiler.D3DDisassemble.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_uint,
                                        ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
    compiler.D3DDisassemble.restype = ctypes.c_long
    status = compiler.D3DDisassemble(data, len(data), 0, None, ctypes.byref(blob))
    return [] if status == 0 else [f"D3DDisassemble failed with {status & 0xFFFFFFFF:#010x}"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--spirv-val")
    args = parser.parse_args()

    spirv_val = find_spirv_val(args.spirv_val)
    if spirv_val is None:
        print("spirv-val was not found: pass --spirv-val", file=sys.stderr)
        return 2

    failed = 0
    checked = 0
    with tempfile.TemporaryDirectory() as work:
        for path in sorted(SPIRV_DIR.glob("resolve_*_cs.h")):
            checked += 1
            problems = check_spirv(path, spirv_val, Path(work))
            print(f"{'FAIL' if problems else 'ok  '} spirv {path.name}")
            for problem in problems:
                print(f"       {problem}")
            failed += bool(problems)
    for path in sorted(DXBC_DIR.glob("resolve_*_cs.h")):
        checked += 1
        problems = check_dxbc(path)
        print(f"{'FAIL' if problems else 'ok  '} dxbc  {path.name}")
        for problem in problems:
            print(f"       {problem}")
        failed += bool(problems)

    print(f"{checked} arrays, {failed} failed")
    return 1 if failed or checked == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
