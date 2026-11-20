#!/bin/sh
# When SuperDrastic does not become ready, the runner starts plain DraStic. DraStic's SDL turns SIGTERM
# into a quit event DraStic ignores, so a shutdown or the frontend ending the game has to kill the process.
# This runs the runner with a DraStic that ignores TERM and checks that a TERM to the runner stops it.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
RUNNER=$ROOT/runner/superdrastic-run
TMP=$(mktemp -d)
trap 'kill -9 $RP $DP 2>/dev/null || true; rm -rf "$TMP"' EXIT
RP= DP=

cat > "$TMP/drastic" << 'EOF'
#!/bin/sh
# SDL's SIGTERM handler posts a quit event and does not exit. This stands in for that.
trap '' TERM INT HUP
if [ -n "$LD_PRELOAD" ]; then
    echo passthrough > "$DSFLIP_STATE"
    sleep 30
    exit 0
fi
echo plain > "$MARKER"
sleep 30
EOF
chmod +x "$TMP/drastic"
echo 'void dsf_term_test(void) {}' | gcc -shared -fPIC -x c - -o "$TMP/libsuperdrastic.so"

export DSFLIP_LIB=$TMP/libsuperdrastic.so
export DSFLIP_STATE=$TMP/state
export DSFLIP_DATA=$TMP/data
export DSFLIP_LOGDIR=$TMP/logs
export DSFLIP_HANDOVER=none
export DSFLIP_GPU_CLOCKS=0
export DSFLIP_AUDIO_RATE=0
export DSFLIP_SHADER=none
export DSFLIP_FALLBACK=1
MARKER=$TMP/marker
export MARKER

"$RUNNER" "$TMP/drastic" "$TMP/game.nds" >"$TMP/runner.out" 2>"$TMP/runner.err" &
RP=$!

i=0
while [ $i -lt 50 ]; do
    grep -q "running DraStic without SuperDrastic" "$TMP/logs/run.log" 2>/dev/null && break
    sleep 0.1
    i=$((i + 1))
done
grep -q "running DraStic without SuperDrastic" "$TMP/logs/run.log"

# the shell must still be the runner when the fallback game is up (exec would have replaced it)
i=0
while [ $i -lt 30 ] && [ ! -f "$MARKER" ]; do sleep 0.1; i=$((i + 1)); done
[ -f "$MARKER" ]
case $(tr '\0' ' ' < /proc/$RP/cmdline) in
    *superdrastic-run*) ;;
    *) echo "runner exec'd DraStic; a TERM can no longer kill it"; exit 1 ;;
esac

kill -TERM $RP
i=0
while [ $i -lt 20 ] && kill -0 $RP 2>/dev/null; do sleep 0.1; i=$((i + 1)); done
if kill -0 $RP 2>/dev/null; then
    echo "runner still alive 2 s after TERM"
    exit 1
fi
rc=0
wait $RP || rc=$?
[ "$rc" = 143 ]
# the fallback game must be gone too (SIGKILL, which the ignored TERM cannot block)
left=0
for p in $(pgrep -f "$TMP/drastic" || true); do
    [ "$p" = "$$" ] && continue
    left=1
done
[ "$left" = 0 ]
echo "runner-term-test: TERM stopped the fallback game"
