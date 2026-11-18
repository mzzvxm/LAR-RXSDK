# Prebuilt SDKs for the LARecomp launcher

The SDK of each new head of the `LARecomp` branch is built on a PC with the scripts in `tools/prebuilt-local` (Windows,
and Linux in WSL or on a Linux PC) and published as release assets. The LARecomp launcher's setup downloads that file
instead of compiling the SDK on the player's PC (the SDK is about half of the build time on a 16-thread PC, and much
more on a weaker one). `tools/prebuilt-local/README.md` says how to run them.

Nothing runs on GitHub Actions: this branch has no workflow (the ones it had were removed on 2026-11-08, the SDK's own
lint, build and nightly workflows with them; they stay on `main`). The scripts add nothing to the SDK's own code.

**Status (2026-11-18): the scripts were adapted to upstream rexglue-sdk v0.11.0 without a build.** v0.11 is a different
SDK shape: about 19 shared libraries instead of `rexruntime` and `rexgpu-xenos` (listed below), Vulkan headers and VMA taken from
a Vulkan SDK on the build PC (no Vulkan submodules), `REXGLUE_USE_VULKAN` gone, SDL3 as a shared library. The launcher
facts this page repeats (`SdkStep`, `LinuxBuild`, the required files) have to follow; they are listed at the end.

## When it is built

By hand, after each push to the `LARecomp` branch: the launcher looks for the release of the head commit, so a head
without one (a change to the docs only, say) has the SDK built on the player's PC until its release is there. Builds
for different commits are independent and each has its own release; a second upload for the same commit replaces its
files.

## What it builds

| | Windows | Linux x86-64 |
|---|---|---|
| Built on | a Windows PC | Linux (WSL is fine): the kit's sysroot sets the glibc floor, not the machine |
| Toolchain | llvm-mingw 20250709 (UCRT, x86-64): Clang 20, lld, libc++, mingw-w64 | the launcher's Linux build kit, revision 5: LLVM 20.1.8 + libc++, an Ubuntu 22.04 sysroot (glibc 2.35) |
| Tools | CMake 4.4.4, Ninja 1.13.2 | the same |
| Configure | `cmake --preset win-amd64`, Tracy off, tests off, no package registry write, the Vulkan SDK's headers (`Vulkan_INCLUDE_DIR`, `Vulkan_LIBRARY`), Release | `cmake --preset linux-amd64`, the same options, `CMAKE_SYSROOT` and the LLVM `ar` family, Vulkan headers from `VULKAN_INCLUDE_DIR` |
| Vulkan | a Vulkan SDK 1.4.313 or newer on the build PC (`Include\vulkan`, `Include\vma`, `Lib\vulkan-1.lib`); headers only, nothing is shipped | the same headers (a Windows Vulkan SDK's `Include` folder is fine); the kit's sysroot has 1.3.204, too old |
| Archive | `.zip` | `.tar.br` |
| Toolchain label | `llvm-mingw-20250709` | `linux-kit-5` |

The pins (URL and SHA-256 of every tool) and the configure arguments are the launcher's own, copied step for step, so the
SDK is the one the player's PC would have built. For Windows that is `Setup/Build/SdkStep.cs` and
`Setup/Resources/tools.json` in the launcher; for Linux `Setup/Linux/LinuxBuild.cs` (`SdkConfigureArguments`,
`ToolchainArguments`), `LinuxToolchain.cs` (`ClangConfig`, the environment) and `Setup/Resources/tools-linux.json`.

The Linux kit is pinned in `tools/prebuilt-local/build-linux.sh` (nothing to set up in the repository): the two pieces of the pre-release
`linux-kit-5` of this repository (`mzzvxm/LAR-RXSDK`; it was on `mzzvxm/LARecomp` until 2026-11-09), checked by SHA-256 and size after the download.

| Piece | File | Size | SHA-256 |
|---|---|---|---|
| LLVM | `llvm-20.1.8-k5-linux-x86_64.tar.br` | 99,417,064 | `633e3eb608a163ce28e0f81175a8ff21ca8ced5db1aed4f1a0413e68168d35df` |
| Sysroot | `sysroot-jammy-k5-linux-x86_64.tar.br` | 44,213,084 | `d174b23dce1887cdc9f85d91523851791910ad016277dd9e440335afeb418a39` |

Like the launcher, the Windows build makes the two edits that belong to submodules of other projects (FFmpeg's `config.h`
is MSVC's configuration, and git on Windows checks libmspack's symlinks out as small text files), turns the install's
`reg add` (the CMake package registry) into an echo, and copies `libc++.dll` and `libunwind.dll` beside the binaries. The
Linux build copies `libc++.so.1`, `libc++abi.so.1` and `libatomic.so.1` (which libc++ needs) into `lib/` as real files; there is
no libunwind on Linux, libc++ and libc++abi use the system's `libgcc_s.so.1` (kit revision 5).

The commit built must contain the llvm-mingw build support for the Windows build and whatever LARecomp needs from the
public SDK. In v0.11 the llvm-mingw support is the `MINGW` lines in the top `CMakeLists.txt` (DLL file names without the `lib`
prefix, `_WIN32_WINNT=0x0A00`, `-fms-extensions`, `-Werror` off), `-municode` for a windowed host in `cmake/rexglue_target.cmake`
and no `-fasync-exceptions` in `cmake/rexglue_recomp_options.cmake`, plus the D3D12 header declarations mingw-w64 lacks
(`src/gpu/xenos/host/d3d12/d3d12_api.h`), the `chrono` stand-ins libc++ needs and the XInput `GetProcAddress` casts. The old
`.def` that exported the `__imp_*` symbols (`cmake/mingw_imp_exports.cmake`) is gone: v0.11 exports its kernel entry points with
`__declspec(dllexport)`. None of it has been built with v0.11. The Windows build fails when preparing the sources if FFmpeg's
`config.h` is not the text it expects.

## The release and its files

Each commit gets one release, tagged `sdk-<the first 12 digits of the commit>` (for example `sdk-5934a85bc89f`), marked
as a pre-release, titled "Prebuilt SDK <12 digits>". Its text says what it is and that the launcher downloads it. The
upload makes the release (and the tag, on the built commit) if it is not there.

| File | What |
|---|---|
| `lar-rxsdk-prebuilt-<12>-windows-x86_64-llvm-mingw-20250709.zip` | the Windows SDK |
| `lar-rxsdk-prebuilt-<12>-linux-x86_64-linux-kit-5.tar.br` | the Linux SDK |
| `<either file>.sha256` | its SHA-256 (`sha256sum` format) |
| `sdk-prebuilt-<12>.json` | the entries for the launcher's release manifest, uploaded last |

`sdk-prebuilt-<12>.json` is the launcher's `sdkPrebuilt` (Windows) and `sdkPrebuiltLinux` entries, for the platforms that
were built:

```json
{
  "sdkPrebuilt": {
    "asset": "lar-rxsdk-prebuilt-5934a85bc89f-windows-x86_64-llvm-mingw-20250709.zip",
    "url": "https://github.com/mzzvxm/LAR-RXSDK/releases/download/sdk-5934a85bc89f/<asset>",
    "sha256": "<64 hex digits>",
    "size": 19827983,
    "sdkCommit": "<the 40-digit commit>",
    "toolchain": "llvm-mingw-20250709"
  },
  "sdkPrebuiltLinux": { "...": "the same fields, toolchain linux-kit-5" }
}
```

The json is the last thing a run adds, so **a release without it is not finished** (or one of its jobs failed before the
end). A re-run for the same commit removes the old json first, rebuilds, replaces the two archives (`--clobber`) and adds
the new json. One platform that fails does not stop the other: the json then has the entry of the one that was built (the
run itself is red).

A `prepare` job fixes the commit (both builds use the same one) and makes the release; the two builds run in parallel and
upload their files; a last job writes the json (and shows it in the run's summary). Nothing is changed in the repository.
Every push leaves one release behind, about 20 MB for Windows and the same order for Linux; delete old ones by hand when
they are no longer wanted (nothing deletes them).

## What is in an archive

The SDK's install folder, at the root of the archive (the launcher unpacks it into `sdk/`), plus `lar-sdk-bundle.json`:

```json
{
  "what": "LAR-RXSDK install built with llvm-mingw",
  "sdkCommit": "<the 40-digit commit>",
  "toolchain": "llvm-mingw-20250709",
  "builtAt": "2026-11-08T10:00:00.0000000Z",
  "options": "REXGLUE_ENABLE_TRACY=OFF REXGLUE_BUILD_TESTS=OFF (Vulkan and Direct3D 12 are always built), Release"
}
```

Each job also checks the files the launcher requires before it packs, that the binaries start (`rexglue --version`, with
only the system's own folders on the search path), and, on Linux, that no link in the install points outside it. The
files, for v0.11 (read from `cmake/rexglue_export_targets.cmake`; Windows names; on Linux `lib/lib<name>.so*` in `lib/`):

| What | Files |
|---|---|
| the codegen tool | `bin/rexglue.exe` |
| libraries a program links (6) | `rexbase`, `rexppc`, `rexkernel`, `rexxam`, `rexxbdm`, `rexapp`: `bin/<name>.dll` and `lib/<name>.dll.a` |
| provider modules, loaded by name at run time (12 on Windows, 10 on Linux) | `rexfilesystem`, `rexapu`, `rexhid`, `rexui`, `rexgpu`, `rexui-sdl3`, `rexapu-sdl3`, `rexhid-sdl3`, `rexgpu-xenos-null`, `rexgpu-xenos-vulkan`, and on Windows `rexgpu-xenos-d3d12`, `rexhid-xinput`: `bin/<name>.dll` only |
| SDL3 (shared) | `bin/SDL3.dll`, `lib/cmake/SDL3/SDL3Config.cmake` (a warning if missing: the names are not read from an install yet) |
| package files | `lib/cmake/rexglue/` `rexglueConfig.cmake`, `rexglueTargets.cmake`, `rexglueModules-Release.cmake`, `rexglue_project.cmake` and the other helper files; `include/rex/` |
| the C++ runtime | Windows `bin/libc++.dll`, `bin/libunwind.dll`; Linux `lib/libc++.so.1`, `lib/libc++abi.so.1`, `lib/libatomic.so.1` |

Windows also checks that every DLL of the install imports only a Windows DLL, an API-set stub or another DLL of the install
(the gcc runtime DLLs would be missing on a player's PC). `TracyClient` is not built (Tracy off). The static `rexxenos` library
(`lib/librexxenos.a`) is installed too.

Why `.tar.br` on Linux: the launcher reads it with .NET alone (Brotli and tar), the same way as the kit's own pieces,
which keeps file modes and links (a zip keeps neither, so the launcher would have to mark programs executable), and it
was measured about a third smaller than gzip -9 on the kit's own pieces (136 MB together against 196 MB). It is packed
the way the kit's `make-kit.sh` packs: `tar | brotli -q 11 -w 24`.

Sizes, measured on the v0.10 Windows install built with the same toolchain: 1,199 files, 102.9 MB installed, **18.9 MB as a
zip** (Deflate, best). The v0.11 install (about 19 DLLs) and the Linux SDK have not been measured; expect the same order, plus
about 2 MB of libc++.

## How the launcher finds it

For the SDK commit it is going to use (on the Experimental channel, the head of `LARecomp`), the launcher

1. takes the first 12 digits of the commit and asks for the release tagged `sdk-<12 digits>` of `mzzvxm/LAR-RXSDK`, and
   for its file `sdk-prebuilt-<12 digits>.json` (by tag, not "latest": these are pre-releases);
2. if the release or the file is not there, builds the SDK on the PC as before;
3. otherwise takes `sdkPrebuilt` (Windows) or `sdkPrebuiltLinux` (Linux) from the json, which has the same fields as the
   manifest entry of the same name, downloads the archive from its `url`, checks its size and SHA-256, unpacks it into
   `sdk/` and reads `lar-sdk-bundle.json`.

It refuses the SDK, and the setup builds nothing from it, when

- the `toolchain` is not the one this PC's setup uses (`llvm-mingw-<version>` of its tool table; `linux-kit-<n>` of the
  kit's `kit.version`), because the program is built with that toolchain afterwards and the two must match (an SDK made
  with the kit's libc++ does not go with a program built against the system's libstdc++, so on Linux the prebuilt SDK is
  used only with the kit), or
- the `sdkCommit` is not the commit it asked for.

## What to set up (Mz)

- **Nothing.** No Actions, no repository variables, no secrets. The uploads use the uploader's own `gh` sign-in (push
  rights on `mzzvxm/LAR-RXSDK`).
- **The Linux kit release must stay public** (`linux-kit-5` on `mzzvxm/LAR-RXSDK`), with the two files above under the
  same names; the Linux script downloads them without signing in.

## Keeping it in step with the launcher

The scripts in `tools/prebuilt-local` repeat these launcher facts; change them together:

- the tool pins (llvm-mingw, CMake, Ninja: `tools.json` and `tools-linux.json`) and the Linux kit's pieces (`kit.version`,
  the `llvm` and `sysroot` URL, SHA-256 and size), in the pins at the top of the two build scripts; a new kit revision means new pins
  here and a new build of every SDK the launcher should find;
- the configure arguments and options (the launcher's `SdkStep` `Options` text and recipe `sdk-recipe-1`), the clang
  configuration files (`clang-config-2`: `--unwindlib=libgcc`) and the build environment of the kit;
- the files the launcher requires (`SdkStep.WindowsFiles`, `LinuxBuild.SdkRequiredFiles`) and the bundle file name
  (`lar-sdk-bundle.json`);
- the release tag (`sdk-<12 digits>`) and the json name (`sdk-prebuilt-<12 digits>.json`), which the launcher looks for.
- v0.11 only: the launcher's own SDK build (when no prebuilt SDK is found) has to give CMake the Vulkan headers 1.4.313+ and VMA
  (`Vulkan_INCLUDE_DIR`, plus a `Vulkan_LIBRARY` file that exists although nothing links it), drop `REXGLUE_USE_VULKAN`, and pass
  `REXGLUE_BUILD_TESTS=OFF` and `REXGLUE_REGISTER_PACKAGE=OFF`; the recipe id (`sdk-recipe-1`) changes with that. The
  prebuilt SDK needs none of this on the player's PC.
- v0.11 only: a program built against the SDK finds the SDK's libraries by `find_package(rexglue 0.11.0 CONFIG)`, which in turn
  runs `find_dependency(SDL3 CONFIG)`: the SDK's prefix has to be on `CMAKE_PREFIX_PATH` (the launcher already does that) and
  SDL3's package files have to be in the install.
