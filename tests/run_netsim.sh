#!/usr/bin/env bash
#
# run_netsim.sh — two-process integration test of the lockstep core.
#
# Launches netsim.exe twice under wine (a host and a guest) so they exchange
# real UDP frames through Netcode_GetInput_Net(), then verifies the lockstep
# guarantee by diffing their per-frame output. See tests/netsim.cpp for what
# each side does.
#
# Prereqs: build.sh has produced build/netsim.exe, and wine + wine32:i386 are
# installed. Usage:  tests/run_netsim.sh [frames] [delay]
#
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/build"
NETSIM="$BUILD/netsim.exe"
FRAMES="${1:-300}"
DELAY="${2:-2}"

WINE="$(command -v wine || true)"
[ -z "$WINE" ] && [ -x /usr/lib/wine/wine ] && WINE=/usr/lib/wine/wine
if [ -z "$WINE" ]; then echo "wine not found — cannot run integration test" >&2; exit 77; fi
[ -f "$NETSIM" ] || { echo "missing $NETSIM — run ./build.sh first" >&2; exit 1; }

export WINEDEBUG=-all
fails=0

# Compare the (frame,word) columns of two CSVs; optionally also require a sync
# column expectation. $1 host.csv  $2 guest.csv  $3 label
diff_words() {
    local hc="$1" gc="$2" label="$3"
    # strip comment lines + header, keep frame,word (cols 1,2)
    local hw gw
    # mingw/wine writes CRLF; strip CR so column compares are byte-clean.
    hw="$(tr -d '\r' < "$hc" | grep -vE '^#|^frame' | cut -d, -f1,2)"
    gw="$(tr -d '\r' < "$gc" | grep -vE '^#|^frame' | cut -d, -f1,2)"
    if [ "$hw" = "$gw" ]; then
        echo "  [PASS] $label: host & guest computed identical merged words ($(echo "$hw" | wc -l) frames)"
    else
        echo "  [FAIL] $label: merged words diverged"
        diff <(echo "$hw") <(echo "$gw") | head -8 | sed 's/^/      /'
        fails=$((fails+1))
    fi
}

# Run a host+guest pair. $1 desyncAt (-1 = none) $2 out-prefix
run_pair() {
    local desync="$1" prefix="$2"
    local hc="$BUILD/${prefix}_host.csv" gc="$BUILD/${prefix}_guest.csv"
    rm -f "$hc" "$gc"
    ( cd "$BUILD" && "$WINE" netsim.exe host  "$hc" "$FRAMES" "$DELAY" "$desync" ) 2>/dev/null &
    local hp=$!
    ( cd "$BUILD" && "$WINE" netsim.exe guest "$gc" "$FRAMES" "$DELAY" "$desync" ) 2>/dev/null &
    local gp=$!
    wait $hp; wait $gp
}

echo "=== lockstep integration test ($FRAMES frames, delay=$DELAY) ==="

# --- case 1: no desync — words identical AND sync stays 1 on both sides ---
echo "[case 1] clean run (seeds agree)"
run_pair -1 clean
if [ ! -s "$BUILD/clean_host.csv" ] || [ ! -s "$BUILD/clean_guest.csv" ]; then
    echo "  [FAIL] one side produced no output"; fails=$((fails+1))
else
    diff_words "$BUILD/clean_host.csv" "$BUILD/clean_guest.csv" "merged words"
    # both sides should report sync==1 for every frame
    bad=$(tr -d '\r' < "$BUILD/clean_host.csv" | grep -vE '^#|^frame' | awk -F, '$3!=1' | wc -l)
    bad2=$(tr -d '\r' < "$BUILD/clean_guest.csv" | grep -vE '^#|^frame' | awk -F, '$3!=1' | wc -l)
    if [ "$bad" -eq 0 ] && [ "$bad2" -eq 0 ]; then
        echo "  [PASS] sync held (==1) on both sides for all frames"
    else
        echo "  [FAIL] sync dropped unexpectedly (host=$bad guest=$bad2 bad frames)"; fails=$((fails+1))
    fi
fi

# --- case 2: guest perturbs its seed at frame K — detector must fire ---
K=$((FRAMES/2))
echo "[case 2] induced desync at frame $K (guest seed perturbed)"
run_pair "$K" desync
if [ ! -s "$BUILD/desync_host.csv" ] || [ ! -s "$BUILD/desync_guest.csv" ]; then
    echo "  [FAIL] one side produced no output"; fails=$((fails+1))
else
    # words must STILL be identical (merge is independent of the seed check)
    diff_words "$BUILD/desync_host.csv" "$BUILD/desync_guest.csv" "merged words (unaffected by seed)"
    # sync must be 1 before K and drop to 0 by K+delay on the host side
    pre=$(tr -d '\r' < "$BUILD/desync_host.csv" | grep -vE '^#|^frame' | awk -F, -v k="$K" '$1<k && $3!=1' | wc -l)
    post=$(tr -d '\r' < "$BUILD/desync_host.csv" | grep -vE '^#|^frame' | awk -F, -v k="$K" -v d="$DELAY" '$1>=k+d && $3==0' | wc -l)
    if [ "$pre" -eq 0 ] && [ "$post" -gt 0 ]; then
        echo "  [PASS] seed-mismatch oracle: sync==1 before frame $K, flipped to 0 after (host saw $post desync frames)"
    else
        echo "  [FAIL] desync detector misbehaved (pre-K bad=$pre, post-K desync-frames=$post)"; fails=$((fails+1))
    fi
fi

echo "=== $([ $fails -eq 0 ] && echo 'ALL PASS' || echo 'FAILURES') ($fails failure$([ $fails -eq 1 ] || echo s)) ==="
exit $([ $fails -eq 0 ] && echo 0 || echo 1)
