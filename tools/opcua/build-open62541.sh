#!/usr/bin/env bash
# Cross-compile open62541 into a static library for the LOGO!'s Cortex-M4F.
#
# Produces  dist/opcua/<profile>/libopen62541.a  plus its headers, and prints
# the section sizes — the numbers the OPC-UA plan's memory budget is built on.
#
# Usage:
#   tools/opcua/build-open62541.sh [profile]
#
#   profile = minimal   UA_NAMESPACE_ZERO=MINIMAL                    (default)
#             trimmed   MINIMAL + no nodeset descriptions
#             none      UA_NAMESPACE_ZERO=NONE (needs an external ROM nodestore)
#
# Why a script and not a README: the option set IS the measurement.  Half of
# these flags exist to keep code out of the image, so a build with a different
# set produces a number that means nothing, and "roughly what we used last
# time" is not reproducible.  The open62541 version is pinned for the same
# reason.
set -euo pipefail

UA_VERSION="v1.5.8"
PROFILE="${1:-minimal}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="${OPCUA_WORK_DIR:-$REPO_ROOT/.opcua-build}"
SRC="$WORK/open62541"
BUILD="$WORK/build-$PROFILE"
OUT="$REPO_ROOT/dist/opcua/$PROFILE"

# The core's own compiler.  Deliberately not whatever is on PATH: the archive
# has to match the sketch's ABI, and its size has to reflect the compiler we
# actually ship.
TOOLCHAIN_BIN="${ARM_TOOLCHAIN_BIN:-$HOME/Library/Arduino15/packages/energia/tools/arm-none-eabi-gcc/8.3.1-20190703/bin}"
if [ ! -x "$TOOLCHAIN_BIN/arm-none-eabi-gcc" ]; then
  echo "error: arm-none-eabi-gcc not found under $TOOLCHAIN_BIN" >&2
  echo "       install the autonomylogic:tm4c core, or set ARM_TOOLCHAIN_BIN" >&2
  exit 1
fi

mkdir -p "$WORK"
if [ ! -d "$SRC/.git" ]; then
  echo "==> cloning open62541 $UA_VERSION"
  git clone --depth 1 --branch "$UA_VERSION" --recurse-submodules --shallow-submodules \
      https://github.com/open62541/open62541.git "$SRC"
else
  echo "==> reusing $SRC ($(git -C "$SRC" describe --tags 2>/dev/null || echo unknown))"
fi

# ---------------------------------------------------------------------------
# Patches.
#
# Applied to a pristine checkout every time (reset first), so the build is
# reproducible and a half-applied tree cannot silently change a measurement.
# Each patch carries its own rationale and its own measured justification —
# see tools/opcua/patches/.
# ---------------------------------------------------------------------------
echo "==> applying patches"
git -C "$SRC" checkout -- . 2>/dev/null || true
for patch in "$REPO_ROOT"/tools/opcua/patches/*.patch; do
  [ -e "$patch" ] || continue
  if git -C "$SRC" apply --check "$patch" 2>/dev/null; then
    git -C "$SRC" apply "$patch"
    echo "    applied $(basename "$patch")"
  else
    echo "error: $(basename "$patch") does not apply to $UA_VERSION" >&2
    echo "       refusing to build — a skipped patch would silently change the" >&2
    echo "       footprint this script exists to measure." >&2
    exit 1
  fi
done

# ---------------------------------------------------------------------------
# The option set.
#
# UA_ARCHITECTURE=none: the core in src/ is OS-independent, and everything
#   platform-specific sits behind the EventLoop / ConnectionManager plugins.
#   We supply those (arch/arduino), so open62541 must not compile its own.
#   Unresolved clock/eventloop symbols in a static library are expected and
#   fine — they resolve at the sketch link.
#
# Everything else is off because it is code we would otherwise pay flash for
# and never call: no JSON/XML encoding (binary only), no PubSub, no discovery,
# no historizing, no method calls, no diagnostics, no runtime node management
# (the address space is fixed at compile time), no auditing.
# UA_LOGLEVEL=600 drops the log format strings, which are pure .rodata.
# ---------------------------------------------------------------------------
CMAKE_ARGS=(
  "-DCMAKE_TOOLCHAIN_FILE=$REPO_ROOT/tools/opcua/cortex-m4.cmake"
  "-DARM_TOOLCHAIN_BIN=$TOOLCHAIN_BIN"
  "-DCMAKE_BUILD_TYPE=MinSizeRel"
  "-DBUILD_SHARED_LIBS=OFF"
  "-DUA_ARCHITECTURE=none"
  "-DUA_ENABLE_AMALGAMATION=OFF"
  "-DUA_BUILD_EXAMPLES=OFF"
  "-DUA_BUILD_UNIT_TESTS=OFF"
  "-DUA_BUILD_TOOLS=OFF"
  "-DUA_ENABLE_ENCRYPTION=OFF"
  "-DUA_ENABLE_SUBSCRIPTIONS=OFF"
  "-DUA_ENABLE_SUBSCRIPTIONS_EVENTS=OFF"
  "-DUA_ENABLE_METHODCALLS=OFF"
  "-DUA_ENABLE_HISTORIZING=OFF"
  "-DUA_ENABLE_DISCOVERY=OFF"
  "-DUA_ENABLE_DIAGNOSTICS=OFF"
  "-DUA_ENABLE_NODEMANAGEMENT=OFF"
  "-DUA_ENABLE_AUDITING=OFF"
  "-DUA_ENABLE_JSON_ENCODING=OFF"
  "-DUA_ENABLE_XML_ENCODING=OFF"
  "-DUA_ENABLE_PUBSUB=OFF"
  "-DUA_ENABLE_DA=OFF"
  # DATATYPES_ALL and TYPEDESCRIPTION are ON against our own interest, because
  # upstream v1.5.8 does not build without them on this option set:
  #   DATATYPES_ALL=OFF -> src/server/ua_server_internal.h and ua_server_async.h
  #     reference UA_CallMethodRequest / UA_Argument unconditionally, so the
  #     generated types they need are gone and five TUs fail to compile.
  #   TYPEDESCRIPTION=OFF -> src/ua_types_definition.c reads
  #     UA_DataTypeMember.memberName, a field that only exists when the option
  #     is ON.
  # Both cost flash we would rather not spend (member-name strings are pure
  # .rodata). Revisit on a future release, or carry a patch — but measure with
  # them ON, because that is what actually links today.
  "-DUA_ENABLE_DATATYPES_ALL=ON"
  "-DUA_ENABLE_TYPEDESCRIPTION=ON"
  "-DUA_ENABLE_STATUSCODE_DESCRIPTIONS=OFF"
  # NOTE: no -DCMAKE_C_FLAGS here. It would override CMAKE_C_FLAGS_INIT and
  # strip the ABI flags. The shim include path (for deps/musl_inet_pton.c's
  # <sys/socket.h>) is set inside the toolchain file for that reason.
  "-DUA_LOGLEVEL=600"
  "-DUA_MULTITHREADING=0"
)

case "$PROFILE" in
  minimal) CMAKE_ARGS+=( "-DUA_NAMESPACE_ZERO=MINIMAL" ) ;;
  trimmed) CMAKE_ARGS+=( "-DUA_NAMESPACE_ZERO=MINIMAL"
                         "-DUA_ENABLE_NODESET_COMPILER_DESCRIPTIONS=OFF" ) ;;
  none)    CMAKE_ARGS+=( "-DUA_NAMESPACE_ZERO=NONE" ) ;;
  *) echo "error: unknown profile '$PROFILE' (minimal|trimmed|none)" >&2; exit 2 ;;
esac

echo "==> configuring [$PROFILE]"
rm -rf "$BUILD" && mkdir -p "$BUILD"
cmake -S "$SRC" -B "$BUILD" "${CMAKE_ARGS[@]}" > "$BUILD/configure.log" 2>&1 || {
  echo "configure FAILED — tail of $BUILD/configure.log:" >&2
  tail -30 "$BUILD/configure.log" >&2
  exit 1
}

echo "==> building"
cmake --build "$BUILD" --parallel "$(sysctl -n hw.ncpu 2>/dev/null || nproc)" \
      > "$BUILD/build.log" 2>&1 || {
  echo "build FAILED — tail of $BUILD/build.log:" >&2
  tail -40 "$BUILD/build.log" >&2
  exit 1
}

LIB="$(find "$BUILD" -name 'libopen62541.a' | head -1)"
[ -n "$LIB" ] || { echo "error: libopen62541.a not produced" >&2; exit 1; }

mkdir -p "$OUT/include"
cp "$LIB" "$OUT/"
# Public headers + the generated ones (config.h / nodeids / types are generated
# into the build tree, not the source tree).
cp -R "$SRC/include/open62541" "$OUT/include/" 2>/dev/null || true
# plugins/include holds server_config_default.h, nodestore_default.h,
# accesscontrol_default.h and the securitypolicy headers — the plugin surface
# the sketch actually calls into, so it is part of the deliverable, not an extra.
cp -R "$SRC/plugins/include/open62541/." "$OUT/include/open62541/" 2>/dev/null || true
find "$BUILD" -path '*/src_generated/open62541/*' -name '*.h' -exec cp {} "$OUT/include/open62541/" \; 2>/dev/null || true

# Guard against the failure mode above: confirm a real object in the archive
# was built Thumb/hard-float before reporting any size from it.
ABI_PROBE="$("$TOOLCHAIN_BIN/arm-none-eabi-readelf" -A "$LIB" 2>/dev/null | grep -c "Tag_ABI_VFP_args: VFP registers")"
if [ "$ABI_PROBE" -eq 0 ]; then
  echo "error: archive was NOT built with the hard-float ABI — it will not link" >&2
  echo "       against the sketch and its size is meaningless. Check that no" >&2
  echo "       -DCMAKE_C_FLAGS override reached the configure step." >&2
  exit 1
fi

echo
echo "==> $PROFILE: $OUT/libopen62541.a  (ABI verified: Thumb, VFP register args)"
"$TOOLCHAIN_BIN/arm-none-eabi-size" -t "$LIB" | tail -1 | \
  awk '{printf "    text=%s  data=%s  bss=%s  total=%s bytes\n", $1, $2, $3, $4}'
echo "    (archive total; the linker's --gc-sections keeps only what the sketch reaches)"
