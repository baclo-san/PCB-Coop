#!/usr/bin/env bash
# build.sh — Linux/web-session build of the PCB co-op artifacts (mirror of build.ps1).
#
# th07.exe is a 32-bit PE, so the DLLs + injector must be i686. On Linux we use the
# distro mingw-w64 cross toolchain (i686-w64-mingw32-gcc/g++). Install it with:
#     apt-get install -y mingw-w64        # provides i686-w64-mingw32-{gcc,g++}
#
# Outputs (in build/):
#   th07_harness.dll   th07_coop.dll   injector.exe   netloop_test.exe (Windows)
#   merge_test_native  (a HOST-native binary: runs the merge unit test here, no wine)
#
# Usage:  ./build.sh            # build everything
#         ./build.sh --clean    # wipe build/ first
#         ./build.sh --test     # build, then run the native merge test
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build"
OBJ="$BUILD/obj"
MH="$ROOT/third_party/minhook"

CC=${CC:-i686-w64-mingw32-gcc}
CXX=${CXX:-i686-w64-mingw32-g++}
HOSTCXX=${HOSTCXX:-g++}          # native compiler for the platform-independent test

CLEAN=0; RUNTEST=0
for a in "$@"; do
    case "$a" in
        --clean) CLEAN=1 ;;
        --test)  RUNTEST=1 ;;
        *) echo "unknown arg: $a" >&2; exit 2 ;;
    esac
done

if ! command -v "$CC" >/dev/null 2>&1; then
    echo "ERROR: $CC not found. Install the 32-bit mingw toolchain:" >&2
    echo "    apt-get install -y mingw-w64" >&2
    exit 1
fi

[ "$CLEAN" = 1 ] && rm -rf "$BUILD"
mkdir -p "$OBJ"

CFLAGS=(-m32 -O2 -Wall)

echo "Toolchain: $($CC --version | head -1)"

# ---- 1. MinHook (C) ----
MH_SRCS=(src/buffer.c src/hook.c src/trampoline.c src/hde/hde32.c src/hde/hde64.c)
MH_OBJS=()
for s in "${MH_SRCS[@]}"; do
    o="$OBJ/$(basename "${s%.c}").o"
    echo "  CC  $s"
    "$CC" "${CFLAGS[@]}" -I"$MH/include" -c "$MH/$s" -o "$o"
    MH_OBJS+=("$o")
done

# ---- 2. harness DLL ----
echo "  LD  th07_harness.dll"
"$CC" "${CFLAGS[@]}" -I"$MH/include" -shared \
    "$ROOT/src/harness/harness.c" "${MH_OBJS[@]}" \
    -o "$BUILD/th07_harness.dll" -static -lkernel32 -luser32

# ---- 2b. coop DLL (P2 entity graft) ----
echo "  LD  th07_coop.dll"
"$CC" "${CFLAGS[@]}" -I"$MH/include" -shared \
    "$ROOT/src/coop/coop.c" "${MH_OBJS[@]}" \
    -o "$BUILD/th07_coop.dll" -static -lkernel32 -luser32

# ---- 3. injector EXE ----
echo "  LD  injector.exe"
"$CC" "${CFLAGS[@]}" "$ROOT/src/injector/injector.c" \
    -o "$BUILD/injector.exe" -static -lkernel32

# ---- 3b. netcode self-test (C++ Windows: STL + Winsock) ----
echo "  CXX netloop_test.exe"
"$CXX" "${CFLAGS[@]}" -Wextra -std=c++17 \
    "$ROOT/tests/netloop_test.cpp" \
    "$ROOT/src/netplay/netcode.cpp" \
    "$ROOT/src/netplay/Connection.cpp" \
    "$ROOT/src/netplay/merge.cpp" \
    "$ROOT/src/netplay/netcode_c_api.cpp" \
    -o "$BUILD/netloop_test.exe" -static -lws2_32

# ---- 3c. native merge test (platform-independent — runs HERE, no wine) ----
echo "  CXX merge_test_native (host $HOSTCXX)"
"$HOSTCXX" -O2 -Wall -Wextra -std=c++17 \
    "$ROOT/tests/merge_test.cpp" "$ROOT/src/netplay/merge.cpp" \
    -o "$BUILD/merge_test_native"

# ---- 4. config template ----
INI="$BUILD/harness.ini"
if [ ! -f "$INI" ]; then
    cat > "$INI" <<'EOF'
[harness]
; mode = record  -> pass real input through, write input_log.bin + sync_record.csv
; mode = replay  -> feed input_log.bin back in, write sync_replay_<pid>.csv
mode = record
; fixed RNG seed forced on first input poll (hex or decimal). Must match across
; both record and replay runs for the desync diff to be meaningful.
seed = 0x1234
EOF
    echo "  GEN harness.ini"
fi

# ---- 5. verify 32-bit (PE machine field at e_lfanew+4 must be 0x14c) ----
pe_machine() {
    python3 - "$1" <<'PY'
import struct, sys
b = open(sys.argv[1], "rb").read()
pe = struct.unpack_from("<i", b, 0x3C)[0]
m  = struct.unpack_from("<H", b, pe + 4)[0]
print({0x14c: "x86 (32-bit) OK", 0x8664: "x64 (WRONG!)"}.get(m, hex(m)))
PY
}
echo ""
echo "Built:"
arch_fail=0
for f in th07_harness.dll th07_coop.dll injector.exe netloop_test.exe; do
    m="$(pe_machine "$BUILD/$f")"
    printf "  %-22s %8d bytes  %s\n" "$f" "$(stat -c%s "$BUILD/$f")" "$m"
    case "$m" in *"32-bit) OK"*) ;; *) arch_fail=1 ;; esac
done
[ "$arch_fail" = 0 ] || { echo "ERROR: one or more artifacts are not 32-bit PEs" >&2; exit 1; }

if [ "$RUNTEST" = 1 ]; then
    echo ""
    echo "Running native merge test:"
    "$BUILD/merge_test_native"
fi

echo ""
echo "Note: the DLLs/injector target Windows; deploy to the th07.exe host to run."
echo "      netloop_test.exe needs Windows/wine; merge_test_native runs here."
