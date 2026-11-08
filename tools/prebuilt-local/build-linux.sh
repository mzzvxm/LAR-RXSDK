#!/usr/bin/env bash
# NOTE (2026-11-08): the GitHub workflow this was first written to copy (.github/workflows/prebuilt-sdk.yaml) was removed; nothing runs on
# Actions. Where a comment says "the workflow", read: the job this step stands for. The pins here are the source of truth.
# build-linux.sh <commit> <outdir>
#
# Builds the prebuilt Linux SDK of one commit on this PC (in WSL), as the "linux" job of
# .github/workflows/prebuilt-sdk.yaml does. The workflow is the source of truth: this script repeats its steps with
# the same pins, the same kit, the same clang.cfg, the same configure line, the same checks and the same file
# names, so the .tar.br made here is the one the workflow would have made. It exists because the workflow could
# not run (the account's GitHub Actions were locked); the result is uploaded by hand with upload.ps1 (Windows side).
# If the workflow changes, change this file with it (the pins and the strings that end up in the bundle are
# checked against the workflow file at the start).
#
#   <commit>   the 40-digit hash (or a name the clone knows: a branch as origin/<name> or <name>)
#   <outdir>   receives lar-rxsdk-prebuilt-<12>-linux-x86_64-linux-kit-5.tar.br, its .sha256 and fragment-linux.json.
#              A Windows folder is fine (/mnt/c/...), and it can be the same folder build-windows.ps1 writes to.
#
# Needs in WSL: bash, git, curl, tar, unzip, brotli, python3, coreutils (all present in Ubuntu 26.04 here; no jq
# and no gh: json is written with python3, and the upload is done from Windows).
#
# Environment (all optional):
#   KIT                where the build kit is unpacked. Default /opt/larecomp-kit as in the workflow when this user
#                      can make it (directly or with sudo -n), else $HOME/larecomp-kit (only the kit's own clang.cfg
#                      and the rpath of the SDK's build-time libraries name this folder: nothing the launcher reads)
#   WORK_BASE          where the checkout and the build go (default $HOME/prebuilt-sdk-work; keep it on the Linux
#                      file system, a /mnt/c build is many times slower). Several GB.
#   CACHE_DIR          downloaded kit pieces (default $HOME/.cache/larecomp-prebuilt-sdk)
#   LOCAL_KIT_DIR      a folder that may already hold the kit pieces under their published names (default
#                      $HOME/kit/out, where make-kit.sh leaves them); a file there is used only when its SHA-256 and
#                      size are the pinned ones, else it is ignored and the piece is downloaded
#   SDK_REPO_URL       the repository to clone (default https://github.com/mzzvxm/LAR-RXSDK.git; not a local
#                      partial clone, which cannot serve its missing files)
#   SUBMODULE_DONOR    a checkout of the SDK with its submodules (git folders under .git/modules) whose objects are
#                      borrowed so that the submodules are not downloaded again (default /mnt/c/mcv/base/sdk-src when
#                      it exists; set it empty to turn this off)
#   JOBS               compiler jobs (cmake --parallel); default Ninja's own choice. The WSL VM has half the PC's RAM
#                      by default: if the build runs out of memory, set a lower number.
#   CLEAN=1            delete the work folder of this commit first (without it a finished checkout is reused)
#   SKIP_START_CHECK=1 do not fail when `rexglue --version` does not start here (see the step; say so when you use it)
#
# Host: the workflow runs on ubuntu-22.04; this runs on whatever the WSL distro is (Ubuntu 26.04 here). The kit brings
# its own compiler, C++ library and sysroot (an Ubuntu 22.04 sysroot, glibc 2.35), and the SDK is compiled and linked
# against that sysroot, so the host's distro should not change the result.
# CHECK: the kit's own tools (clang, lld, cmake, ninja) run on the host, so the host needs a glibc at least as new as the
# sysroot's (26.04 has 2.42) and whatever shared libraries the kit's clang needs (the "Try the kit" step finds out);
# and `rexglue --version` is run on the host at the end, which needs libgcc_s.so.1 and the SDK's other libraries there.

set -euo pipefail
shopt -s dotglob nullglob

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

if [ $# -ne 2 ]; then
  sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2
  exit 2
fi
COMMIT_ARG=$1
mkdir -p "$2"
OUTDIR=$(cd "$2" && pwd)

# ---- The workflow's pins (the "linux" job's env block). Keep in step with prebuilt-sdk.yaml. ----
TRIPLE=x86_64-unknown-linux-gnu
CMAKE_URL=https://github.com/Kitware/CMake/releases/download/v4.4.4/cmake-4.4.4-linux-x86_64.tar.gz
CMAKE_SHA256=e5bb807f7728cb60cd8b27ebc97a2edb469b68655f21e844a600c3575b76f5bb
NINJA_URL=https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-linux.zip
NINJA_SHA256=5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6
KIT_VERSION=5
KIT_LLVM_URL=https://github.com/mzzvxm/LARecomp/releases/download/linux-kit-5/llvm-20.1.8-k5-linux-x86_64.tar.br
KIT_LLVM_SHA256=633e3eb608a163ce28e0f81175a8ff21ca8ced5db1aed4f1a0413e68168d35df
KIT_LLVM_SIZE=99417064
KIT_SYSROOT_URL=https://github.com/mzzvxm/LARecomp/releases/download/linux-kit-5/sysroot-jammy-k5-linux-x86_64.tar.br
KIT_SYSROOT_SHA256=d174b23dce1887cdc9f85d91523851791910ad016277dd9e440335afeb418a39
KIT_SYSROOT_SIZE=44213084

# The repository that owns the release, and the strings the workflow writes into the bundle.
SDK_REPO=mzzvxm/LAR-RXSDK
BUNDLE_WHAT="LAR-RXSDK install built with the Linux kit"
BUNDLE_OPTIONS="REXGLUE_USE_VULKAN=ON REXGLUE_ENABLE_TRACY=OFF (FidelityFX off, its default), Release"
CFG_COMMENT="# Written by the prebuilt-sdk workflow as the LARecomp launcher writes it (clang-config-2). Do not edit."
ASSET_TEMPLATE='lar-rxsdk-prebuilt-${SHORT}-linux-x86_64-${toolchain}.tar.br'   # as written in the workflow (literal)

# ---- Settings ----
SDK_REPO_URL=${SDK_REPO_URL:-https://github.com/mzzvxm/LAR-RXSDK.git}
WORK_BASE=${WORK_BASE:-${HOME}/prebuilt-sdk-work}
CACHE_DIR=${CACHE_DIR:-${HOME}/.cache/larecomp-prebuilt-sdk}
LOCAL_KIT_DIR=${LOCAL_KIT_DIR-${HOME}/kit/out}
SUBMODULE_DONOR=${SUBMODULE_DONOR-/mnt/c/mcv/base/sdk-src}
CLEAN=${CLEAN:-0}
SKIP_START_CHECK=${SKIP_START_CHECK:-0}
JOBS=${JOBS:-}

err() { echo "error: $*" >&2; }
die() { err "$*"; exit 1; }

PHASE_LINES=()
PHASE_NAME=""
PHASE_START=0
end_phase() {
  if [ -n "${PHASE_NAME}" ]; then
    PHASE_LINES+=("$(printf '%-52s %7d s' "${PHASE_NAME}" "$((SECONDS - PHASE_START))")")
    PHASE_NAME=""
  fi
}
phase() {
  end_phase
  echo
  echo "=== $1  ($(date +%H:%M:%S))"
  PHASE_NAME=$1
  PHASE_START=${SECONDS}
}

# ---------------------------------------------------------------------------------------------------------------
phase "1/8 pins"
# The pins above are the source of truth: the branch has no workflow (removed 2026-11-08, no GitHub Actions).
echo "Pins: kit ${KIT_VERSION}, CMake and Ninja as above."

# ---------------------------------------------------------------------------------------------------------------
# A clean environment: the runner's PATH has no Windows folders in it and none of the build variables below.
PATH=$(printf '%s' "${PATH}" | tr ':' '\n' | grep -v '^/mnt/' | paste -sd: -)
export PATH
unset CC CXX CFLAGS CXXFLAGS CPPFLAGS LDFLAGS CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH \
      CMAKE_GENERATOR CMAKE_TOOLCHAIN_FILE CMAKE_PREFIX_PATH CMAKE_BUILD_PARALLEL_LEVEL PKG_CONFIG_PATH || true
for tool in git curl tar unzip brotli python3 sha256sum realpath find; do
  command -v "${tool}" > /dev/null || die "${tool} is not installed in this WSL distro (sudo apt-get install ${tool})."
done
echo "host: $(. /etc/os-release && echo "${PRETTY_NAME}"), $(nproc) processors, $(free -g | awk '/^Mem:/ {print $2}') GB RAM"

# The kit's folder (see KIT above).
kit_usable() { [ -d "$1" ] && [ -w "$1" ]; }
if [ -z "${KIT:-}" ]; then
  if kit_usable /opt/larecomp-kit      || { mkdir -p /opt/larecomp-kit 2> /dev/null && kit_usable /opt/larecomp-kit; }      || { sudo -n mkdir -p /opt/larecomp-kit 2> /dev/null && sudo -n chown "$(id -u):$(id -g)" /opt/larecomp-kit 2> /dev/null && kit_usable /opt/larecomp-kit; }; then
    KIT=/opt/larecomp-kit
  else
    KIT="${HOME}/larecomp-kit"
    echo "note: /opt/larecomp-kit cannot be made without a password; the kit goes to ${KIT}."
  fi
fi
mkdir -p "${KIT}"
kit_usable "${KIT}" || die "the kit folder ${KIT} is not writable (set KIT to a folder of yours)."
echo "kit folder: ${KIT}"

WORK="${WORK_BASE}/$(printf '%s' "${COMMIT_ARG}" | tr -c 'A-Za-z0-9._-' '_')"
SRC="${WORK}/src"
STATE="${WORK}/state"
TMPROOT="${WORK}/tmp"
BUILD="${WORK}/build"
SDK="${WORK}/install"
if [ "${CLEAN}" = 1 ]; then
  rm -rf "${WORK}"
fi
mkdir -p "${WORK}" "${STATE}" "${TMPROOT}"

# ---------------------------------------------------------------------------------------------------------------
# actions/checkout with fetch-depth 0, fetch-tags and submodules: recursive. Blobless: the whole history and every
# tag (the SDK's version number is made from them), file contents only for what is checked out; the same tree.
phase "2/8 checkout of the commit and its submodules"
export GIT_TERMINAL_PROMPT=0

resolve_commit() { # <repo> <name>: the 40-digit hash, or nothing
  local candidate result
  for candidate in "$2" "origin/$2"; do
    if result=$(git -C "$1" rev-parse --verify --quiet "${candidate}^{commit}" 2> /dev/null) && [[ "${result}" =~ ^[0-9a-f]{40}$ ]]; then
      echo "${result}"
      return 0
    fi
  done
  return 1
}

if [ ! -f "${STATE}/checkout.ok" ]; then
  rm -rf "${WORK}"
  mkdir -p "${WORK}" "${STATE}" "${TMPROOT}"
  git clone --filter=blob:none --no-checkout --quiet "${SDK_REPO_URL}" "${SRC}"

  sha=$(resolve_commit "${SRC}" "${COMMIT_ARG}") || sha=""
  if [ -z "${sha}" ] && [[ "${COMMIT_ARG}" =~ ^[0-9a-f]{40}$ ]]; then
    # A commit that no branch or tag holds: GitHub serves it by its hash.
    git -C "${SRC}" fetch --quiet origin "${COMMIT_ARG}"
    sha=$(resolve_commit "${SRC}" "${COMMIT_ARG}") || sha=""
  fi
  [ -n "${sha}" ] || die "the commit '${COMMIT_ARG}' is not in ${SDK_REPO_URL} (is it pushed? a local-only commit cannot be built from here)."

  git -C "${SRC}" -c advice.detachedHead=false checkout --quiet --detach "${sha}"

  # Borrow the objects of a checkout that has the submodules already (nothing is downloaded twice; what it does not
  # have is fetched from the network by git as usual): submodule.alternateLocation=superproject makes git look for
  # <donor>/.git/modules/<submodule>/objects.
  if [ -n "${SUBMODULE_DONOR}" ] && [ -d "${SUBMODULE_DONOR}/.git/objects" ] && [ -d "${SUBMODULE_DONOR}/.git/modules" ]; then
    echo "${SUBMODULE_DONOR}/.git/objects" > "${SRC}/.git/objects/info/alternates"
    git -C "${SRC}" config submodule.alternateLocation superproject
    git -C "${SRC}" config submodule.alternateErrorStrategy info
    echo "Submodule objects are borrowed from ${SUBMODULE_DONOR}."
  else
    echo "No usable submodule donor: the submodules are downloaded."
  fi

  git -C "${SRC}" submodule sync --recursive --quiet
  git -C "${SRC}" submodule update --init --force --recursive --jobs 4 --quiet

  # Every submodule must be at the commit the SDK pins ('-' = not checked out, '+' = another commit, 'U' = conflict).
  badsub=$(git -C "${SRC}" submodule status --recursive | grep -E '^[-+U]' || true)
  [ -z "${badsub}" ] || die "submodules are not at their pinned commits:"$'\n'"${badsub}"

  echo "${sha}" > "${STATE}/checkout.ok"
else
  sha=$(head -n 1 "${STATE}/checkout.ok")
  asked=$(resolve_commit "${SRC}" "${COMMIT_ARG}") || asked=""
  if [ -n "${asked}" ] && [ "${asked}" != "${sha}" ]; then die "${WORK} holds ${sha}, not ${asked}. Use CLEAN=1."; fi
  if [ -z "${asked}" ] && [[ "${COMMIT_ARG}" =~ ^[0-9a-f]{40}$ ]] && [ "${COMMIT_ARG}" != "${sha}" ]; then die "${WORK} holds ${sha}, not ${COMMIT_ARG}. Use CLEAN=1."; fi
  echo "The checkout of ${sha} from an earlier run is reused."
fi

# The workflow's "Check out the commit that was asked for".
head_sha=$(git -C "${SRC}" rev-parse HEAD)
[ "${head_sha}" = "${sha}" ] || die "HEAD is ${head_sha}, expected ${sha}."
git -C "${SRC}" log -1 --format='%H %s'
SDK_COMMIT=${sha}
SHORT=${sha:0:12}
RELEASE_TAG="sdk-${SHORT}"
toolchain="linux-kit-${KIT_VERSION}"
asset="lar-rxsdk-prebuilt-${SHORT}-linux-x86_64-${toolchain}.tar.br"
echo "Release tag ${RELEASE_TAG}, asset ${asset}"

# ---------------------------------------------------------------------------------------------------------------
# What the launcher's Tools step does (LinuxToolsStep), as the workflow does it: the pieces are downloaded, checked
# against their SHA-256, unpacked (an archive with one top folder loses it), and clang.cfg / clang++.cfg are written
# beside the compiler (ClangConfig, recipe clang-config-2: --unwindlib=libgcc, GCC's libgcc_s.so.1 is the one
# unwinder; the kit (revision 5 and later) carries no libunwind).
phase "3/8 the kit (LLVM, sysroot, CMake, Ninja)"

file_matches() { # <file> <sha256> [size]
  if [ -n "${3:-}" ] && [ "$(stat -c %s "$1")" != "$3" ]; then return 1; fi
  [ "$(sha256sum "$1" | cut -d' ' -f1)" = "$2" ]
}

# fetch <url> <sha256> [size]: the path of a verified file on stdout. A verified copy in the cache or in LOCAL_KIT_DIR
# is used as it is; otherwise the file is downloaded into the cache. Inside $(...) bash drops "set -e", so every
# failure here ends the function by hand.
fetch() {
  local url=$1 sha=${2,,} size=${3:-} name file candidate
  name=$(basename "${url%%\?*}")
  file="${CACHE_DIR}/${name}"
  for candidate in "${file}" "${LOCAL_KIT_DIR:+${LOCAL_KIT_DIR}/${name}}"; do
    if [ -n "${candidate}" ] && [ -f "${candidate}" ] && file_matches "${candidate}" "${sha}" "${size}"; then
      echo "using ${candidate} (SHA-256 checked)" >&2
      echo "${candidate}"
      return 0
    fi
  done
  mkdir -p "${CACHE_DIR}"
  echo "downloading ${url}" >&2
  if ! curl --fail --silent --show-error --location --retry 5 --retry-delay 5 --output "${file}.part" "${url}"; then
    err "Downloading ${url} failed." >&2
    exit 1
  fi
  mv "${file}.part" "${file}"
  if [ -n "${size}" ] && [ "$(stat -c %s "${file}")" != "${size}" ]; then
    err "${url} is $(stat -c %s "${file}") bytes, expected ${size}." >&2
    exit 1
  fi
  if ! echo "${sha}  ${file}" | sha256sum --check - >&2; then
    err "${url} does not have the SHA-256 ${sha}." >&2
    exit 1
  fi
  echo "${file}"
}

# unpack <archive> <folder>
unpack() {
  local part="$2.part"
  rm -rf "${part}" "$2"
  mkdir -p "${part}"
  case "$1" in
    *.tar.br) brotli --decompress --stdout "$1" | tar -x -C "${part}" ;;
    *.tar.gz) tar -xzf "$1" -C "${part}" ;;
    *.zip) unzip -q "$1" -d "${part}" ;;
    *) die "Unknown archive type: $1" ;;
  esac
  local entries=("${part}"/*)
  if [ ${#entries[@]} -eq 1 ] && [ -d "${entries[0]}" ] && [ ! -L "${entries[0]}" ]; then
    mv "${entries[0]}" "$2"
    rmdir "${part}"
  else
    mv "${part}" "$2"
  fi
}

kit_pins="k${KIT_VERSION} ${KIT_LLVM_SHA256} ${KIT_SYSROOT_SHA256} ${CMAKE_SHA256} ${NINJA_SHA256}"
if [ -f "${KIT}/.pins" ] && [ "$(cat "${KIT}/.pins")" = "${kit_pins}" ]; then
  echo "The kit in ${KIT} is the pinned one (revision ${KIT_VERSION}); it is reused."
else
  rm -f "${KIT}/.pins"
  # (assigned first so that a failed download stops the step)
  archive=$(fetch "${KIT_LLVM_URL}" "${KIT_LLVM_SHA256}" "${KIT_LLVM_SIZE}"); unpack "${archive}" "${KIT}/llvm"
  archive=$(fetch "${KIT_SYSROOT_URL}" "${KIT_SYSROOT_SHA256}" "${KIT_SYSROOT_SIZE}"); unpack "${archive}" "${KIT}/sysroot"
  archive=$(fetch "${CMAKE_URL}" "${CMAKE_SHA256}"); unpack "${archive}" "${KIT}/cmake"
  archive=$(fetch "${NINJA_URL}" "${NINJA_SHA256}"); unpack "${archive}" "${KIT}/ninja"
  chmod +x "${KIT}/ninja/ninja"
fi

# CHECK: the files the launcher's tools-linux.json checks for, and the ones it marks executable.
for needed in llvm/bin/clang llvm/bin/clang++ llvm/bin/ld.lld llvm/bin/llvm-ar llvm/bin/llvm-ranlib llvm/bin/llvm-nm llvm/bin/llvm-strip llvm/bin/llvm-objcopy \
              sysroot/hostbin/pkg-config sysroot/hostbin/wayland-scanner cmake/bin/cmake ninja/ninja; do
  [ -x "${KIT}/${needed}" ] || die "The kit is not as expected: ${needed} is missing or not executable."
done
for lib in libc++.so.1 libc++abi.so.1 libatomic.so.1; do
  [ -e "${KIT}/llvm/lib/${TRIPLE}/${lib}" ] || die "The kit has no ${lib}."
done
# One unwinder per process, GCC's (libgcc_s.so.1 from the system): a kit that still carries LLVM's libunwind is an
# older revision (k4 and before), whose libc++abi unwinds with it and hangs a thread that ends in pthread_exit.
if compgen -G "${KIT}/llvm/lib/${TRIPLE}/libunwind*" > /dev/null; then
  die "The kit still has libunwind: it is a kit older than revision 5 (one unwinder, libgcc_s.so.1). Pin revision 5 or later."
fi
echo "${kit_pins}" > "${KIT}/.pins"

cfg() {
cat <<EOF
${CFG_COMMENT}
--sysroot=${KIT}/sysroot
-fuse-ld=lld
--rtlib=compiler-rt
--unwindlib=libgcc
-Wno-unused-command-line-argument
EOF
}
cfg > "${KIT}/llvm/bin/clang.cfg"
{ cfg; echo "-stdlib=libc++"; } > "${KIT}/llvm/bin/clang++.cfg"

# What the launcher puts in the environment of a build with the kit (LinuxToolPaths.BuildPath / BuildVariables).
# (The workflow also sets TMPDIR to the runner's temp folder here; this script does it below.)
cat > "${KIT}/env.sh" <<EOF
export PATH="${KIT}/sysroot/hostbin:${KIT}/llvm/bin:${KIT}/cmake/bin:${KIT}/ninja:\${PATH}"
export LD_LIBRARY_PATH="${KIT}/llvm/lib/${TRIPLE}\${LD_LIBRARY_PATH:+:\${LD_LIBRARY_PATH}}"
export PKG_CONFIG_SYSROOT_DIR="${KIT}/sysroot"
export PKG_CONFIG_LIBDIR="${KIT}/sysroot/usr/lib/x86_64-linux-gnu/pkgconfig:${KIT}/sysroot/usr/lib/pkgconfig:${KIT}/sysroot/usr/share/pkgconfig"
unset PKG_CONFIG_PATH
EOF

# ---------------------------------------------------------------------------------------------------------------
# The launcher's own try-out of the kit (LinuxKitProbe): a C++23 program with <format> builds and runs, and
# pkg-config finds GTK 3 in the sysroot.
phase "4/8 try the kit"
# shellcheck disable=SC1091
source "${KIT}/env.sh"
export TMPDIR="${TMPROOT}"
printf '#include <cstdio>\n#include <format>\nint main() { std::puts(std::format("kit-ok {}", 42).c_str()); return 0; }\n' > "${TMPROOT}/probe.cpp"
clang++ -std=c++23 "${TMPROOT}/probe.cpp" -o "${TMPROOT}/probe"
"${TMPROOT}/probe"
pkg-config --exists gtk+-3.0
clang++ --version
cmake --version
ninja --version

# ---------------------------------------------------------------------------------------------------------------
# The launcher's configure line (LinuxBuild.SdkConfigureArguments + ToolchainArguments with the kit).
phase "5/8 configure, build, install"
jobs_args=()
if [ -n "${JOBS}" ]; then jobs_args=(--parallel "${JOBS}"); fi

cd "${SRC}"
echo "--- Configure"
cmake --preset linux-amd64 \
  -B "${BUILD}" \
  -DCMAKE_INSTALL_PREFIX="${SDK}" \
  -DREXGLUE_USE_VULKAN=ON \
  -DREXGLUE_ENABLE_TRACY=OFF \
  -DCMAKE_EXPORT_NO_PACKAGE_REGISTRY=ON \
  -DCMAKE_C_COMPILER="${KIT}/llvm/bin/clang" \
  -DCMAKE_CXX_COMPILER="${KIT}/llvm/bin/clang++" \
  -DCMAKE_MAKE_PROGRAM="${KIT}/ninja/ninja" \
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF \
  -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DCMAKE_SYSROOT="${KIT}/sysroot" \
  -DCMAKE_AR="${KIT}/llvm/bin/llvm-ar" \
  -DCMAKE_RANLIB="${KIT}/llvm/bin/llvm-ranlib" \
  -DCMAKE_NM="${KIT}/llvm/bin/llvm-nm" \
  -DCMAKE_STRIP="${KIT}/llvm/bin/llvm-strip" \
  -DCMAKE_OBJCOPY="${KIT}/llvm/bin/llvm-objcopy"

echo "--- Build"
cmake --build "${BUILD}" --config Release ${jobs_args[@]+"${jobs_args[@]}"}

# The install registers itself in $HOME/.cmake/packages/rexglue; HOME points at a throwaway folder for it
# (the launcher does the same), so nothing is left in the user's home.
echo "--- Install"
rm -rf "${SDK}"   # no file of an earlier try may end up in the archive
mkdir -p "${TMPROOT}/cmake-home"
HOME="${TMPROOT}/cmake-home" cmake --install "${BUILD}" --config Release

# libc++, libc++abi and libatomic next to the SDK's libraries (LinuxBuild.CxxRuntimeFiles, CopyCxxRuntime): real
# files, the launcher's unpacker refuses links that leave the folder. libatomic.so.1 is there because libc++ needs it
# and not every PC has it. No libunwind: libc++ and libc++abi use the PC's libgcc_s.so.1, the one unwinder.
for lib in libc++.so.1 libc++abi.so.1 libatomic.so.1; do
  cp -L "${KIT}/llvm/lib/${TRIPLE}/${lib}" "${SDK}/lib/${lib}"
done
cd "${WORK}"

# ---------------------------------------------------------------------------------------------------------------
# What the launcher reads (SdkBundleInfo): the toolchain label (linux-kit-<kit version>) and the commit must be
# the ones its setup uses. (The workflow writes this with jq; python3 gives the same json, same key order.)
phase "6/8 bundle info and checks"
BUILT_AT=$(date -u +%Y-%m-%dT%H:%M:%S.0000000Z)
WHAT="${BUNDLE_WHAT}" SDK_COMMIT="${SDK_COMMIT}" TOOLCHAIN="${toolchain}" BUILT_AT="${BUILT_AT}" OPTIONS="${BUNDLE_OPTIONS}" \
python3 -I -c '
import json, os, sys
info = {"what": os.environ["WHAT"], "sdkCommit": os.environ["SDK_COMMIT"], "toolchain": os.environ["TOOLCHAIN"],
        "builtAt": os.environ["BUILT_AT"], "options": os.environ["OPTIONS"]}
json.dump(info, sys.stdout, indent=2)
sys.stdout.write("\n")
' > "${SDK}/lar-sdk-bundle.json"
cat "${SDK}/lar-sdk-bundle.json"

# The files the launcher's SDK step requires with the kit (LinuxBuild.SdkRequiredFiles).
for needed in bin/rexglue lib/librexruntime.so lib/librexgpu-xenos.so lib/cmake/rexglue/rexglueConfig.cmake lar-sdk-bundle.json \
              lib/libc++.so.1 lib/libc++abi.so.1 lib/libatomic.so.1; do
  [ -e "${SDK}/${needed}" ] || die "The install has no ${needed}."
done
# and none of libunwind (a program with LLVM's unwinder next to glibc's hangs at exit in pthread_exit)
if compgen -G "${SDK}/lib/libunwind*" > /dev/null; then
  die "The install has a libunwind: the kit's one unwinder is libgcc_s.so.1."
fi

# The launcher's unpacker refuses a link that points outside the SDK folder (or is absolute): none may be there.
root=$(realpath "${SDK}")
bad=0
while IFS= read -r -d '' link; do
  target=$(readlink "${link}")
  resolved=$(realpath -m "$(dirname "${link}")/${target}")
  case "${target}" in
    /*) err "${link} is a link to the absolute path ${target}."; bad=1 ;;
    *) case "${resolved}" in "${root}"/*) ;; *) err "${link} points outside the SDK (${target})."; bad=1 ;; esac ;;
  esac
done < <(find "${SDK}" -type l -print0)
[ "${bad}" = 0 ] || die "the install has links that leave it."

echo "install: $(find "${SDK}" -type f | wc -l) files, $(du -sh "${SDK}" | cut -f1)"

# ---------------------------------------------------------------------------------------------------------------
# CHECK: rexglue --version exits 0 without LD_LIBRARY_PATH (the SDK's rpath $ORIGIN/../lib finds the copied libc++).
phase "7/8 rexglue starts"
if ! env -u LD_LIBRARY_PATH "${SDK}/bin/rexglue" --version; then
  echo "--- the shared libraries rexglue wants and does not find here:" >&2
  env -u LD_LIBRARY_PATH ldd "${SDK}/bin/rexglue" 2>&1 | grep -i 'not found' >&2 || true
  if [ "${SKIP_START_CHECK}" = 1 ]; then
    echo "warning: rexglue --version failed; SKIP_START_CHECK=1, going on. Report this: the workflow's check is fatal." >&2
  else
    die "rexglue --version did not start (the workflow's check is fatal; SKIP_START_CHECK=1 goes on without it)."
  fi
fi

# ---------------------------------------------------------------------------------------------------------------
# The same packing the kit's own pieces use (make-kit.sh: tar | brotli -q 11 -w 24, names under ./, owner root),
# which is what the launcher reads with BrotliStream and TarReader.
phase "8/8 pack (tar | brotli -q 11 -w 24)"
tar -C "${SDK}" --numeric-owner --owner=0 --group=0 -cf - . | brotli -q 11 -w 24 -c > "${OUTDIR}/${asset}"
brotli --test "${OUTDIR}/${asset}"

asset_sha=$(sha256sum "${OUTDIR}/${asset}" | cut -d' ' -f1)
asset_size=$(stat -c %s "${OUTDIR}/${asset}")
echo "${asset_sha}  ${asset}" > "${OUTDIR}/${asset}.sha256"
echo "${asset}: ${asset_size} bytes, sha256 ${asset_sha}"

url="https://github.com/${SDK_REPO}/releases/download/${RELEASE_TAG}/${asset}"
ASSET="${asset}" URL="${url}" SHA256="${asset_sha}" SIZE="${asset_size}" SDK_COMMIT="${SDK_COMMIT}" TOOLCHAIN="${toolchain}" \
python3 -I -c '
import json, os, sys
fragment = {"sdkPrebuiltLinux": {"asset": os.environ["ASSET"], "url": os.environ["URL"], "sha256": os.environ["SHA256"],
                                 "size": int(os.environ["SIZE"]), "sdkCommit": os.environ["SDK_COMMIT"],
                                 "toolchain": os.environ["TOOLCHAIN"]}}
json.dump(fragment, sys.stdout, indent=2)
sys.stdout.write("\n")
' > "${OUTDIR}/fragment-linux.json"

end_phase
echo
echo "=== done"
for line in "${PHASE_LINES[@]}"; do echo "  ${line}"; done
echo "  total $((SECONDS / 60)) min"
echo
echo "Commit:  ${SDK_COMMIT}"
echo "Files:   ${OUTDIR}/${asset}"
echo "         ${OUTDIR}/${asset}.sha256"
echo "         ${OUTDIR}/fragment-linux.json"
echo "Next: manifest.py ${SDK_COMMIT} ${OUTDIR}, then upload.ps1 from Windows (see README.md)."
