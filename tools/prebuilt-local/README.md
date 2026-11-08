# Prebuilt SDKs, built by hand

`.github/workflows/prebuilt-sdk.yaml` builds the SDK for the LARecomp launcher on GitHub's runners and publishes it as the
release `sdk-<first 12 digits of the commit>`. When Actions cannot run (a locked account, say), these scripts do the same
jobs on a PC and upload the same files by hand, so the launcher finds the release exactly as if the workflow had made it.

**The workflow is the source of truth.** The scripts repeat its steps (same pins, same edits, same configure line, same
checks, same file names, same json). If you change one, change the other. Each build script compares its pins and the
texts that end up in the bundle with the workflow file at the start and stops when they differ. What the workflow in turn
repeats from the launcher (`tools.json`, `tools-linux.json`, `SdkStep`, `LinuxBuild`) is listed in `docs/prebuilt-sdk.md`.

| File | What it does | Runs on |
|---|---|---|
| `build-windows.ps1 -Commit <sha> -Out <dir> [-Tools <dir>]` | the workflow's `windows` job: llvm-mingw build, `.zip`, `.sha256` | Windows PowerShell 5.1 |
| `build-linux.sh <sha> <outdir>` | the workflow's `linux` job: the launcher's kit, `.tar.br`, `.sha256` | WSL (Ubuntu 26.04 here) |
| `manifest.py <sha> <outdir>` | the workflow's `manifest` job: `sdk-prebuilt-<12>.json` | Python 3, either side |
| `upload.ps1 -Commit <sha> -Out <dir> [-DryRun]` | makes the release if needed, uploads the files, the json last | Windows, needs `gh` signed in |

## Order

Use the 40-digit commit of the head of the `LARecomp` branch on GitHub (the launcher asks for the release of that commit;
`git ls-remote https://github.com/mzzvxm/LAR-RXSDK.git refs/heads/LARecomp`). It has to be pushed: the scripts clone it from
GitHub, and the release's tag is made on it. Use one short output folder for everything, for example `C:\mcv\pb\<12 digits>`
(FFmpeg's tree has paths near Windows' 260 character limit).

1. **Windows SDK** (nothing else heavy running; a build overlapping a game measurement spoils both):

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File C:\mcv\larecomp\sdk\wt-ci\tools\prebuilt-local\build-windows.ps1 `
     -Commit <40 digits> -Out C:\mcv\pb\<12 digits>
   ```

   `-Tools` defaults to `C:\mcv\base\tools` (llvm-mingw 20250709, CMake 4.4.4, Ninja 1.13.2 are there and are used when their
   folder name and `--version` match the pins; otherwise the pinned file is downloaded and its SHA-256 checked).
   `-Clean` starts from nothing, `-Jobs N` limits the compilers, `-SubmoduleDonor ''` stops it borrowing the submodule
   objects of `C:\mcv\base\sdk-src`.

2. **Linux SDK**, after the Windows build has finished:

   ```bash
   wsl -d Ubuntu -- bash /mnt/c/mcv/larecomp/sdk/wt-ci/tools/prebuilt-local/build-linux.sh <40 digits> /mnt/c/mcv/pb/<12 digits>
   ```

   The kit pieces (LLVM, sysroot, CMake, Ninja) are taken from `~/kit/out` when their SHA-256 and size are the pinned ones
   (they are, as of 2026-11-08, so nothing is downloaded and the not yet public `linux-kit-5` release is not needed for
   this), else downloaded from the pinned URLs. Settings are environment variables, listed at the top of the script
   (`JOBS`, `KIT`, `WORK_BASE`, `CLEAN=1`).

3. **The json**, from either side:

   ```
   python -I C:\mcv\larecomp\sdk\wt-ci\tools\prebuilt-local\manifest.py <40 digits> C:\mcv\pb\<12 digits>
   ```

   It measures the archives, cross-checks the `.sha256` files, the `fragment-*.json` the builds wrote and the commit inside
   the zip's `lar-sdk-bundle.json`, and writes `sdk-prebuilt-<12>.json` with an entry for each platform that is there
   (one platform alone is fine; the launcher builds the other on the player's PC).

4. **Upload**, the main session only (it writes to GitHub). First `-DryRun`, then for real:

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File ...\upload.ps1 -Commit <40 digits> -Out C:\mcv\pb\<12 digits> -DryRun
   ```

   It checks the account's push access and that the commit exists on GitHub, checks the files against the json, makes the
   pre-release `sdk-<12>` (title "Prebuilt SDK <12>", the workflow's text, `--latest=false`, tag on the commit) if it is
   missing, deletes an old `sdk-prebuilt-<12>.json` if the release exists, uploads the archives and `.sha256` files with
   `--clobber`, then the json last, then reads the release back and compares names and sizes.

5. Afterwards delete `<out>\work` (several GB) and, in WSL, `~/prebuilt-sdk-work/<commit>`.

## Time and space (estimates, none of this has been run end to end)

| | Windows | Linux (WSL) |
|---|---|---|
| Clone and submodules | 1 to 5 min (the donor saves the big downloads) | 1 to 5 min |
| Kit | none (the PC's tools) | 2 to 3 min the first time (unpacking LLVM and the sysroot), then reused |
| Configure | about 1 min | 1 to 2 min |
| Build | 3 to 10 min (an earlier llvm-mingw check on this PC timed the configure at 45 s and the build at 47 s, not necessarily from a cold start) | 5 to 15 min |
| Install, checks, archive | about 1 min (a 19 MB zip) | 2 to 3 min (`brotli -q 11` is slow) |
| Disk | 2 to 3 GB in `work`, a 19 MB result | the same, plus about 1.5 GB for the kit |

The WSL VM gets half of the PC's RAM by default; if the Linux build runs out of memory use `JOBS=8` (or lower).

## What differs from the workflow

- **Where it is built**: on this PC, not on a runner. The release text does not say so (it is the workflow's text).
- **Windows tools**: the PC's copies of llvm-mingw, CMake and Ninja when they are the pinned versions (checked by folder
  name and `--version`, not by hash), else the pinned downloads.
- **Windows git**: the PC's git, with `core.longpaths` on and `core.symlinks` off for the run only (the runner's setup);
  nothing in the user's git configuration changes. Line endings follow the PC's system configuration (`core.autocrlf`
  true, like the runner's).
- **The zip** is written entry by entry (forward slashes, Deflate at its best) instead of with `CreateFromDirectory`, so
  that it does not depend on the .NET version; the entry names are checked for backslashes. The `.sha256` files end in a
  plain newline (the workflow's `Set-Content` writes CRLF on Windows).
- **Linux host**: WSL Ubuntu 26.04 instead of the runner's Ubuntu 22.04. The kit brings its own compiler, C++ library and
  sysroot (an Ubuntu 22.04 sysroot, glibc 2.35), so the host should not change the result. The kit goes to
  `/opt/larecomp-kit` when this user can make it (directly, or with `sudo -n`), else `~/larecomp-kit`; the folder only
  appears in the kit's own `clang.cfg` and in the rpath of build-time libraries.
- **The Linux json** is written with `python3` (the WSL distro has no `jq`), the same keys in the same order.
- **Version number**: the SDK's version comes from `git describe` over the tags; the public repository has no tags, so the
  workflow's `fetch-tags` finds none and the launcher's own clone finds none either. These scripts clone the public
  repository too, so the number is the same (`...-dev.unknown`). A build from the local clone `C:\mcv\larecomp\sdk\LAR-RXSDK`
  would show `v0.10.0-87-g...` because that clone also has the upstream tags: that is why the scripts do not use it.

## CHECK (not verified; no build was run)

- Windows: that `rexglue.exe --version` starts with only Windows' folders on `PATH` (the workflow has the same check and
  the same doubt), and that a blobless clone plus `--filter` checkout fetches what the build needs (the launcher clones
  the same way).
- Windows: that the user's `gh` has push access to `mzzvxm/LAR-RXSDK` (`upload.ps1` asks GitHub and stops if not).
- Linux: that the kit's `clang` and `ld.lld` run on Ubuntu 26.04 (the "Try the kit" step builds and runs a `<format>`
  program first), and that `rexglue --version` finds every library it needs on the host; `SKIP_START_CHECK=1` goes on
  when only that last check fails (say so, the workflow's check is fatal).
- Both: borrowing the submodule objects from `C:\mcv\base\sdk-src` (`submodule.alternateLocation`) was tried for two
  submodules; the rest fall back to the network by themselves.
- The pieces that were tested without a build: the pin comparison with the workflow (it refuses a changed pin), the
  FFmpeg and libmspack edits on copies of the real files, the zip writer, the kit pieces' hashes and unpacking.
