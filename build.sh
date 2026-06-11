#!/usr/bin/env bash
#
# build.sh — Linux/mingw build of the PCB co-op harness, co-op DLL, injector,
#            and netcode self-test. The Linux counterpart of build.ps1.
#
# th07.exe is a 32-bit PE, so everything that touches its address space (the
# harness DLL, the co-op DLL, and the cross-process injector) must be i686.
# We use the i686-w64-mingw32 cross toolchain (Debian/Ubuntu package
# gcc-mingw-w64-i686 / g++-mingw-w64-i686). No Windows SDK needed.
#
#   sudo apt-get install -y gcc-mingw-w64-i686 g++-mingw-w64-i686
#   # optional, to RUN the self-test on Linux:
#   sudo dpkg --add-architecture i386 && sudo apt-get update
#   sudo apt-get install -y --no-install-recommends wine wine32:i386
#
# Outputs (in build/):
#   th07_harness.dll   th07_coop.dll   injector.exe   netloop_test.exe
#
# Usage:
#   ./build.sh            # build everything
#   ./build.sh --clean    # wipe build/ first
#   ./build.sh --test     # build, then run netloop_test.exe under wine
#   ./build.sh --clean --test
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build"
OBJ="$BUILD/obj"
MH="$ROOT/third_party/minhook"

CC="${CC:-i686-w64-mingw32-gcc}"
CXX="${CXX:-i686-w64-mingw32-g++}"
CFLAGS=(-m32 -O2 -Wall)

DO_CLEAN=0
DO_TEST=0
for a in "$@"; do
    case "$a" in
        --clean) DO_CLEAN=1 ;;
        --test)  DO_TEST=1 ;;
        *) echo "unknown arg: $a" >&2; exit 2 ;;
    esac
done

command -v "$CC"  >/dev/null || { echo "missing $CC (apt install gcc-mingw-w64-i686)"  >&2; exit 1; }
command -v "$CXX" >/dev/null || { echo "missing $CXX (apt install g++-mingw-w64-i686)" >&2; exit 1; }

[ "$DO_CLEAN" = 1 ] && rm -rf "$BUILD"
mkdir -p "$OBJ"

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

# ---- 3. injector EXE ----
echo "  LD  injector.exe"
"$CC" "${CFLAGS[@]}" "$ROOT/src/injector/injector.c" \
    -o "$BUILD/injector.exe" -static -lkernel32

# ---- 3b. netcode self-test (C++: validates STL + Winsock toolchain) ----
echo "  CXX netloop_test.exe"
"$CXX" "${CFLAGS[@]}" -Wextra -std=c++17 \
    "$ROOT/tests/netloop_test.cpp" \
    "$ROOT/src/netplay/netcode.cpp" \
    "$ROOT/src/netplay/Connection.cpp" \
    -o "$BUILD/netloop_test.exe" -static -lws2_32

# ---- 3c. two-process lockstep integration driver ----
echo "  CXX netsim.exe"
"$CXX" "${CFLAGS[@]}" -Wextra -std=c++17 \
    "$ROOT/tests/netsim.cpp" \
    "$ROOT/src/netplay/netcode.cpp" \
    "$ROOT/src/netplay/Connection.cpp" \
    -o "$BUILD/netsim.exe" -static -lws2_32

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

echo ""
echo "Built:"
for f in th07_harness.dll th07_coop.dll injector.exe netloop_test.exe netsim.exe; do
    printf '  %-22s %8d bytes\n' "$f" "$(stat -c%s "$BUILD/$f")"
done

# ---- 5. optional self-test under wine ----
if [ "$DO_TEST" = 1 ]; then
    echo ""
    echo "Running netloop_test.exe under wine..."
    WINE="$(command -v wine || true)"
    [ -z "$WINE" ] && [ -x /usr/lib/wine/wine ] && WINE=/usr/lib/wine/wine
    if [ -z "$WINE" ]; then
        echo "  wine not found — skipping run (install wine + wine32:i386)" >&2
    else
        ( cd "$BUILD" && WINEDEBUG=-all "$WINE" netloop_test.exe )
        echo ""
        echo "Running two-process lockstep integration test..."
        bash "$ROOT/tests/run_netsim.sh" || echo "  (integration test reported failures)"
    fi
fi
