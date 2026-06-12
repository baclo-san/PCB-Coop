#!/usr/bin/env bash
#
# build.sh — Linux/mingw build of the PCB co-op harness, co-op DLL, injector,
#            netcode integration DLL, and the netcode tests. The Linux
#            counterpart of build.ps1 (which ships the actual play artifacts).
#
# th07.exe is a 32-bit PE, so everything that touches its address space (the
# harness DLL, the co-op DLL, and the cross-process injector) must be i686.
# We use the i686-w64-mingw32 cross toolchain (Debian/Ubuntu package
# gcc-mingw-w64-i686 / g++-mingw-w64-i686, or the mingw-w64 meta-package).
# No Windows SDK needed.
#
#   sudo apt-get install -y mingw-w64
#   # optional, to RUN the Windows-side tests on Linux:
#   sudo dpkg --add-architecture i386 && sudo apt-get update
#   sudo apt-get install -y --no-install-recommends wine wine32:i386
#
# Outputs (in build/):
#   th07_harness.dll   th07_coop.dll   th07_coop_net.dll   injector.exe
#   netloop_test.exe   netsim.exe      (Windows test exes — need wine here)
#   merge_test_native  (HOST-native: runs the merge unit test here, no wine)
#
# Usage:
#   ./build.sh            # build everything
#   ./build.sh --clean    # wipe build/ first
#   ./build.sh --test     # build, run the native merge test, then (if wine is
#                         # available) the netloop + two-process lockstep tests
#   ./build.sh --clean --test
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build"
OBJ="$BUILD/obj"
MH="$ROOT/third_party/minhook"

CC="${CC:-i686-w64-mingw32-gcc}"
CXX="${CXX:-i686-w64-mingw32-g++}"
HOSTCXX="${HOSTCXX:-g++}"        # native compiler for the platform-independent test
CFLAGS=(-m32 -O2 -Wall)

# The netcode core TUs every netcode consumer links. MergeKeys lives in its own
# winsock-free TU (merge.cpp) so it can be unit-tested natively; netcode_c_api.cpp
# is the C-linkage shim coop.c will call (building it everywhere keeps it honest).
NETCODE_SRCS=(
    "$ROOT/src/netplay/netcode.cpp"
    "$ROOT/src/netplay/Connection.cpp"
    "$ROOT/src/netplay/merge.cpp"
    "$ROOT/src/netplay/netcode_c_api.cpp"
)

DO_CLEAN=0
DO_TEST=0
for a in "$@"; do
    case "$a" in
        --clean) DO_CLEAN=1 ;;
        --test)  DO_TEST=1 ;;
        *) echo "unknown arg: $a" >&2; exit 2 ;;
    esac
done

command -v "$CC"  >/dev/null || { echo "missing $CC (apt install mingw-w64)"  >&2; exit 1; }
command -v "$CXX" >/dev/null || { echo "missing $CXX (apt install mingw-w64)" >&2; exit 1; }

[ "$DO_CLEAN" = 1 ] && rm -rf "$BUILD"
mkdir -p "$OBJ"

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

# ---- 2b. co-op DLL (P2 entity graft) ----
echo "  LD  th07_coop.dll"
"$CC" "${CFLAGS[@]}" -I"$MH/include" -shared \
    "$ROOT/src/coop/coop.c" "${MH_OBJS[@]}" \
    -o "$BUILD/th07_coop.dll" -static -lkernel32 -luser32

# ---- 2c. netcode integration DLL (Fork A: wires the netcode into th07) ----
# C++ (links netcode + Connection + MinHook). NOT yet game-tested — see
# docs/th07_fork_a_integration.md.
echo "  LD  th07_coop_net.dll"
"$CXX" "${CFLAGS[@]}" -std=c++17 -I"$MH/include" -I"$ROOT/src/netplay" -shared \
    "$ROOT/src/netplay/coop_net.cpp" "${NETCODE_SRCS[@]}" "${MH_OBJS[@]}" \
    -o "$BUILD/th07_coop_net.dll" -static -lkernel32 -luser32 -lws2_32

# ---- 3. injector EXE ----
echo "  LD  injector.exe"
"$CC" "${CFLAGS[@]}" "$ROOT/src/injector/injector.c" \
    -o "$BUILD/injector.exe" -static -lkernel32

# ---- 3b. netcode self-test (C++: validates STL + Winsock toolchain) ----
echo "  CXX netloop_test.exe"
"$CXX" "${CFLAGS[@]}" -Wextra -std=c++17 \
    "$ROOT/tests/netloop_test.cpp" "${NETCODE_SRCS[@]}" \
    -o "$BUILD/netloop_test.exe" -static -lws2_32

# ---- 3c. two-process lockstep integration driver ----
echo "  CXX netsim.exe"
"$CXX" "${CFLAGS[@]}" -Wextra -std=c++17 \
    "$ROOT/tests/netsim.cpp" "${NETCODE_SRCS[@]}" \
    -o "$BUILD/netsim.exe" -static -lws2_32

# ---- 3d. native merge test (platform-independent — runs HERE, no wine) ----
echo "  CXX merge_test_native (host $HOSTCXX)"
"$HOSTCXX" -O2 -Wall -Wextra -std=c++17 \
    "$ROOT/tests/merge_test.cpp" "$ROOT/src/netplay/merge.cpp" \
    -o "$BUILD/merge_test_native"

# ---- 4. config templates ----
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

NETINI="$BUILD/coop_net.ini"
if [ ! -f "$NETINI" ]; then
    cat > "$NETINI" <<'EOF'
[net]
; role  = host   (listens) | guest (connects to host)
role  = host
; guest only: host's IP to connect to
peer  = 127.0.0.1
; host: port to listen on / guest: host's port
port  = 47000
; guest only: local UDP bind port
local = 47001
; input delay in frames — BOTH sides must use the same value
delay = 2
; shared start RNG seed (host authoritative). Both sides must match until a
; proper seed handshake is implemented (see docs/th07_fork_a_integration.md).
seed  = 0x1234
EOF
    echo "  GEN coop_net.ini"
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
for f in th07_harness.dll th07_coop.dll th07_coop_net.dll injector.exe netloop_test.exe netsim.exe; do
    m="$(pe_machine "$BUILD/$f")"
    printf "  %-22s %8d bytes  %s\n" "$f" "$(stat -c%s "$BUILD/$f")" "$m"
    case "$m" in *"32-bit) OK"*) ;; *) arch_fail=1 ;; esac
done
[ "$arch_fail" = 0 ] || { echo "ERROR: one or more artifacts are not 32-bit PEs" >&2; exit 1; }

# ---- 6. tests ----
if [ "$DO_TEST" = 1 ]; then
    echo ""
    echo "Running native merge test:"
    "$BUILD/merge_test_native"

    echo ""
    WINE="$(command -v wine || true)"
    [ -z "$WINE" ] && [ -x /usr/lib/wine/wine ] && WINE=/usr/lib/wine/wine
    if [ -z "$WINE" ]; then
        echo "wine not found — skipping the Windows-side tests (install wine + wine32:i386)" >&2
    else
        echo "Running netloop_test.exe under wine..."
        ( cd "$BUILD" && WINEDEBUG=-all "$WINE" netloop_test.exe )
        echo ""
        echo "Running two-process lockstep integration test..."
        bash "$ROOT/tests/run_netsim.sh" || echo "  (integration test reported failures)"
    fi
fi

echo ""
echo "Note: the DLLs/injector target Windows; deploy to the th07.exe host to run."
