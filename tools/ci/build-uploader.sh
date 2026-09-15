#!/usr/bin/env bash
# Cross-compile the LOGO uploader for every host the index declares.
# CGO off for static binaries; -trimpath and a zeroed build id for reproducibility.
#
#   tools/ci/build-uploader.sh dist/tools
set -euo pipefail
OUT="${1:-dist/tools}"
mkdir -p "$OUT"
# Absolute: the build runs from the module dir, so a relative -o would misplace it.
OUT="$(cd "$OUT" && pwd)"
here="$(cd "$(dirname "$0")/../logo-upload" && pwd)"

build() {
  local host=$1 goos=$2 goarch=$3 goarm=${4:-} ext=${5:-}
  local dir="$OUT/$host"
  mkdir -p "$dir"
  ( cd "$here" && env GOOS="$goos" GOARCH="$goarch" ${goarm:+GOARM=$goarm} CGO_ENABLED=0 \
      go build -trimpath -buildvcs=false -ldflags "-s -w -buildid=" -o "$dir/logo-upload$ext" . )
  printf "  %-22s %s\n" "$host" "$(ls -lh "$dir/logo-upload$ext" | awk '{print $5}')"
}

echo "==> building logo-upload"
build x86_64-mingw32       windows amd64 ""  .exe
build i686-mingw32         windows 386   ""  .exe
build x86_64-apple-darwin  darwin  amd64 ""  ""
build arm64-apple-darwin   darwin  arm64 ""  ""
build x86_64-pc-linux-gnu  linux   amd64 ""  ""
build aarch64-linux-gnu    linux   arm64 ""  ""
build arm-linux-gnueabihf  linux   arm   7   ""

# darwin/arm64 is SIGKILLed on Apple Silicon without an ad-hoc signature.
if ! python3 - "$OUT/arm64-apple-darwin/logo-upload" <<'PY'
import struct, sys
with open(sys.argv[1], "rb") as f: data = f.read()
magic, _, _, _, ncmds = struct.unpack_from("<IIIII", data, 0)
assert magic == 0xFEEDFACF, f"not a 64-bit Mach-O: 0x{magic:08X}"
off = 32
for _ in range(ncmds):
    cmd, size = struct.unpack_from("<II", data, off)
    if cmd == 0x1D:   # LC_CODE_SIGNATURE
        print("    arm64-apple-darwin: ad-hoc signature present"); sys.exit(0)
    off += size
sys.exit("arm64-apple-darwin binary is UNSIGNED — it will be killed on Apple Silicon")
PY
then exit 1; fi
