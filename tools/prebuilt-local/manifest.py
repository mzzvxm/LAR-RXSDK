#!/usr/bin/env python3
"""Writes sdk-prebuilt-<12>.json, the launcher's manifest entries for one commit's prebuilt SDKs.

This is the "manifest" job of .github/workflows/prebuilt-sdk.yaml, run on the files that are in a folder (the
workflow's source of truth is that file; keep the two in step). It reads the archives that build-windows.ps1 and
build-linux.sh left in <outdir> and writes the same json the workflow's last job writes:

    {"sdkPrebuilt": {...}, "sdkPrebuiltLinux": {...}}      (an entry only for a platform that was built)

Each entry has asset, url (the release asset on mzzvxm/LAR-RXSDK), sha256, size, sdkCommit and toolchain.
The sha256 and size are measured from the file itself, and checked against the file's .sha256 and the
fragment-<platform>.json the build wrote, when they are there. Standard library only.

Usage:  python3 -I manifest.py <40-digit commit> <outdir> [--repo owner/name]
Exit:   0 written, 1 something is wrong (nothing is written then).
"""

import argparse
import hashlib
import json
import os
import re
import sys
import zipfile

# (json key, platform part of the asset name, extension, toolchain label pattern, fragment file)
PLATFORMS = (
    ("sdkPrebuilt", "windows-x86_64", ".zip", r"llvm-mingw-[0-9]+", "fragment-windows.json"),
    ("sdkPrebuiltLinux", "linux-x86_64", ".tar.br", r"linux-kit-[0-9]+", "fragment-linux.json"),
)


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def read_json(path):
    with open(path, "r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def find_asset(outdir, short, platform, extension, toolchain_pattern):
    """The one archive of this platform for this commit, and its toolchain label."""
    pattern = re.compile(
        r"^lar-rxsdk-prebuilt-%s-%s-(%s)%s$" % (short, re.escape(platform), toolchain_pattern, re.escape(extension))
    )
    found = []
    for name in sorted(os.listdir(outdir)):
        match = pattern.match(name)
        if match and os.path.isfile(os.path.join(outdir, name)):
            found.append((name, match.group(1)))
    if len(found) > 1:
        raise SystemExit(
            "More than one %s archive for %s in %s: %s. Keep the one to publish."
            % (platform, short, outdir, ", ".join(name for name, _ in found))
        )
    return found[0] if found else None


def check_sha_file(path, asset, sha256):
    """The .sha256 beside an archive ('<hash>  <name>', sha256sum format), when it is there."""
    sha_path = path + ".sha256"
    if not os.path.isfile(sha_path):
        print("warning: %s has no .sha256 file" % asset, file=sys.stderr)
        return
    with open(sha_path, "r", encoding="utf-8-sig") as handle:
        parts = handle.read().split()
    if len(parts) < 2 or parts[0].lower() != sha256 or parts[1].lstrip("*") != asset:
        raise SystemExit("%s does not agree with %s (it says %s)." % (sha_path, asset, " ".join(parts)))


def check_fragment(outdir, key, fragment_name, entry):
    """The fragment the build wrote (same fields as the entry), when it is there."""
    fragment_path = os.path.join(outdir, fragment_name)
    if not os.path.isfile(fragment_path):
        return
    fragment = read_json(fragment_path).get(key)
    if fragment is None:
        raise SystemExit("%s has no %s entry." % (fragment_name, key))
    for field in ("asset", "sha256", "size", "sdkCommit", "toolchain"):
        if fragment.get(field) != entry[field]:
            raise SystemExit(
                "%s says %s=%r, but the archive says %r. Was the archive rebuilt after the fragment was written?"
                % (fragment_name, field, fragment.get(field), entry[field])
            )


def check_zip_bundle(path, sha, toolchain):
    """A zip's lar-sdk-bundle.json says which commit and toolchain it was built from; the launcher checks that."""
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
        for name in names:
            if "\\" in name:
                raise SystemExit("%s has an entry name with a backslash: %s" % (os.path.basename(path), name))
        if "lar-sdk-bundle.json" not in names:
            raise SystemExit("%s has no lar-sdk-bundle.json at its root." % os.path.basename(path))
        bundle = json.loads(archive.read("lar-sdk-bundle.json").decode("utf-8-sig"))
    if bundle.get("sdkCommit") != sha:
        raise SystemExit("%s was built from %s, not %s." % (os.path.basename(path), bundle.get("sdkCommit"), sha))
    if bundle.get("toolchain") != toolchain:
        raise SystemExit(
            "%s says toolchain %s but its name says %s." % (os.path.basename(path), bundle.get("toolchain"), toolchain)
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("commit", help="the full 40-digit commit hash the SDKs were built from")
    parser.add_argument("outdir", help="the folder with the archives (build-windows.ps1 / build-linux.sh -Out / <outdir>)")
    parser.add_argument("--repo", default="mzzvxm/LAR-RXSDK", help="the repository that holds the release (default %(default)s)")
    parser.add_argument("--server", default="https://github.com", help=argparse.SUPPRESS)
    args = parser.parse_args()

    sha = args.commit.strip().lower()
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise SystemExit("The commit must be the full 40-digit hash (the build scripts print it), not %r." % args.commit)
    short = sha[:12]
    tag = "sdk-" + short
    outdir = os.path.abspath(args.outdir)
    if not os.path.isdir(outdir):
        raise SystemExit("%s is not a folder." % outdir)

    result = {}
    for key, platform, extension, toolchain_pattern, fragment_name in PLATFORMS:
        found = find_asset(outdir, short, platform, extension, toolchain_pattern)
        if found is None:
            print("no %s archive for %s in %s: no %s entry" % (platform, short, outdir, key))
            continue
        asset, toolchain = found
        path = os.path.join(outdir, asset)
        size = os.path.getsize(path)
        sha256 = sha256_of(path)
        if size == 0:
            raise SystemExit("%s is empty." % asset)
        check_sha_file(path, asset, sha256)
        if extension == ".zip":
            check_zip_bundle(path, sha, toolchain)
        entry = {
            "asset": asset,
            "url": "%s/%s/releases/download/%s/%s" % (args.server, args.repo, tag, asset),
            "sha256": sha256,
            "size": size,
            "sdkCommit": sha,
            "toolchain": toolchain,
        }
        check_fragment(outdir, key, fragment_name, entry)
        result[key] = entry
        print("%s: %s (%d bytes, sha256 %s)" % (key, asset, size, sha256))

    if not result:
        raise SystemExit("Nothing was built for %s in %s, so there is nothing to write." % (short, outdir))

    name = "sdk-prebuilt-%s.json" % short
    target = os.path.join(outdir, name)
    with open(target, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(result, handle, indent=2)
        handle.write("\n")
    print("")
    with open(target, "r", encoding="utf-8") as handle:
        print(handle.read(), end="")
    print("wrote %s" % target)
    return 0


if __name__ == "__main__":
    sys.exit(main())
