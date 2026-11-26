#!/usr/bin/env bash
# End-to-end acceptance: **stock omp-cef server component** (official release package, unmodified) x Android client.
#
#   bash tests/cef/e2e_real_plugin.sh
#
# How it differs from verify_all.sh:
#   verify_all.sh  uses the test server in tests/cef/cef_server.cpp that "reimplements the protocol"; it runs fast
#                  and covers every detailed assertion for protocol/input/World2D/WorldObject3D.
#   This script uses components/Cef.dll from the official aurora-mp/omp-cef release
#                  (v1.3.0-x86.zip), installed in a real open.mp server; the client connects with real SA-MP
#                  to join the server, following the exact player path: game connect -> CEF handshake -> create browser
#                  -> page load -> two-way events. This is the final acceptance condition in the task brief.
#
# Dependencies (one-time setup, see AUDIT.md "stage 8, second batch"):
#   D:/server/DAYZSA/omp-cef-build/server    open.mp server + components/Cef.dll + gamemodes/cef_e2e.amx + config.json
#   D:/server/DAYZSA/omp-cef-build/www       home page index.html
#   Ports: game 7877 / CEF 7879 / page HTTP 8000 (not 7777, to avoid clashing with an existing local server)
#
# Modifies servers.json on the device (backed up to servers.json.bak first); the test server stays running in the background.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/out"
SRV="${OMP_CEF_SERVER:-D:/server/DAYZSA/omp-cef-build/server}"
WWW="${OMP_CEF_WWW:-D:/server/DAYZSA/omp-cef-build/www}"
PKG="${1:-com.sampmobile.launcher}"
HOST_IP="${HOST_IP:-192.168.0.117}"
GAME_PORT=7877
CEF_PORT=7879
HTTP_PORT=8000
LOG="$SRV/log.txt"

# Fixed coordinates in the launcher (2388x1080 landscape, see the README screenshot)
# Favorites first row: the "N servers" header always spans 190-227, the first row text block 276-357, centre y=316.
# Writing 186 before hit the header instead, so the tap did nothing and it waited 120s for nothing.
TAP_ROW1="800 316"          # First favorites row (test server)
TAP_ENTER_PANEL="2020 948"  # "Enter Server" at the bottom of the right detail panel
TAP_ENTER_DIALOG="1333 820" # "Enter Server" in the nickname/version confirmation dialog

mkdir -p "$OUT"
say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
adbq() { MSYS_NO_PATHCONV=1 adb "$@"; }
fail() { echo "FAIL  $*"; exit 1; }

# --------------------------------------------------------------- 0. Preflight checks
say "0/5 Preflight checks"
adbq devices | grep -q "device$" || fail "No adb device connected"
[ -f "$SRV/omp-server.exe" ]                  || fail "Missing $SRV/omp-server.exe"
[ -f "$SRV/components/Cef.dll" ]              || fail "Missing $SRV/components/Cef.dll (component from the official release)"
[ -f "$SRV/gamemodes/cef_e2e.amx" ]           || fail "Missing $SRV/gamemodes/cef_e2e.amx (run pawncc under gamemodes first)"
[ -f "$WWW/index.html" ]                      || fail "Missing $WWW/index.html"
# The gamemode must be cef_e2e: if something like login_demo is running instead, the server will not print a single [e2e] line,
# and every assertion below fails together, looking like a client problem when it is just the wrong config.
grep -q '"cef_e2e"' "$SRV/config.json" 2>/dev/null \
    || fail "config.json pawn.main_scripts is not cef_e2e (the current config runs a different gamemode)"
echo "  Component: $(grep -m1 -ao 'Cef Component ([0-9.]*)' "$SRV/log.txt" 2>/dev/null || echo '(see the log after startup)')"
echo "  Server directory: $SRV"

# The component version must exactly equal the protocol version the client declares (plugin.cpp uses ==).
CLIENT_VER=$(sed -n 's/^inline constexpr uint8_t VERSION_\(MAJOR\|MINOR\|PATCH\).*=\s*\([0-9]*\);/\2/p' \
    "$ROOT/app/src/main/cpp/SAMPMOBILE/Cef/CefProtocol.h" | paste -sd. -)
echo "  Client protocol version: ${CLIENT_VER:-?} (the server component must match it, otherwise reason=2 version mismatch)"

# --------------------------------------------------- 1. Clear ports + start the HTTP service
say "1/5 Ports and HTTP"
for p in $GAME_PORT $CEF_PORT; do
    pid=$(netstat -ano -p UDP 2>/dev/null | awk -v p=":$p" '$2 ~ p {print $NF; exit}')
    if [ -n "${pid:-}" ]; then
        echo "  Port $p is held by PID $pid; stopping it (only this test port)"
        taskkill //PID "$pid" //F >/dev/null 2>&1
    fi
done
sleep 2

if ! curl -sS -m 3 -o /dev/null "http://127.0.0.1:$HTTP_PORT/index.html" 2>/dev/null; then
    echo "  Starting the test-page HTTP service :$HTTP_PORT"
    # Must be started as an independent process. Started with cmd /b before, python was reaped as soon as the script exited (or was Ctrl-C'd / killed),
    # so the client then only ever saw CEF's "webpage cannot be opened" error page — and the assertions would only report
    # "never saw PAGE -> SERVER", looking like the client never emits when the page simply never loaded.
    powershell -NoProfile -Command \
        "Start-Process -FilePath 'python' -ArgumentList '-m','http.server','$HTTP_PORT','--bind','0.0.0.0' -WorkingDirectory '$(cygpath -w "$WWW")' -WindowStyle Hidden" \
        >/dev/null 2>&1
    for _ in $(seq 1 20); do
        curl -sS -m 2 -o /dev/null "http://127.0.0.1:$HTTP_PORT/index.html" 2>/dev/null && break
        sleep 0.5
    done
fi
curl -sS -m 3 -o /dev/null -w "  Test page HTTP %{http_code}\n" "http://127.0.0.1:$HTTP_PORT/index.html" \
    || fail "Test page failed to start"

# Confirm once more from the phone: curl working locally does not mean the phone can reach it (firewall Public profile / AP isolation).
# If it cannot connect, only warn — keep going so the log still records the real reason, "page failed to load".
if adbq shell "toybox nc -w 3 $HOST_IP $HTTP_PORT </dev/null" >/dev/null 2>&1; then
    echo "  Phone -> $HOST_IP:$HTTP_PORT reachable"
else
    echo "  [warn] Phone cannot reach $HOST_IP:$HTTP_PORT; the page will show 'webpage cannot be opened' (PAGE -> SERVER is guaranteed to fail)"
fi

# Allow inbound (needs admin; on failure only warn, do not abort)
netsh advfirewall firewall add rule name="omp-cef-e2e-udp" dir=in action=allow \
    protocol=UDP localport=$GAME_PORT,$CEF_PORT profile=any >/dev/null 2>&1 || \
    echo "  [warn] Failed to add the firewall rule; the phone may not be able to connect"
netsh advfirewall firewall add rule name="omp-cef-e2e-http" dir=in action=allow \
    protocol=TCP localport=$HTTP_PORT profile=any >/dev/null 2>&1 || true

# ------------------------------------------------------- 2. Start the open.mp server
say "2/5 Starting open.mp + Cef.dll"
rm -f "$LOG"
( cd "$SRV" && cmd //c "start /b omp-server.exe" >/dev/null 2>&1 )
READY=0
for _ in $(seq 1 40); do
    if grep -q "Legacy Network started on port $GAME_PORT" "$LOG" 2>/dev/null; then READY=1; break; fi
    sleep 0.5
done
[ $READY -eq 1 ] || { tail -30 "$LOG" 2>/dev/null; fail "Server did not start (see the log above)"; }
grep -E "Cef Component|CEF\] Network Server|\[e2e\] gamemode ready" "$LOG" | sed 's/^/  /'

# ------------------------------------------------- 3. On device: favorites + join the server
say "3/5 Device: write favorite and join the server"
# Only use evidence from "this section": a leftover client connection from the previous round auto-reconnects as soon as the server comes up, so asserting only on
# order of appearance would PASS before any tap — which would be validating the previous round. The app is force-stopped below,
# so the old connection drops with it, and setting SINCE to just after the tap is cleanest.
adbq shell "cp /sdcard/Android/data/$PKG/files/servers.json \
            /sdcard/Android/data/$PKG/files/servers.json.bak" >/dev/null 2>&1 || true
cat > "$OUT/servers-e2e.json" <<EOF
{
    "servers": [
        { "host": "$HOST_IP", "port": "$GAME_PORT" },
        { "host": "192.168.0.117", "port": "7777" },
        { "host": "116.62.52.151", "port": "7777" }
    ]
}
EOF
adbq push "$(cygpath -w "$OUT/servers-e2e.json")" \
    "/sdcard/Android/data/$PKG/files/servers.json" >/dev/null 2>&1 \
    || fail "Failed to push servers.json (adbq disables path conversion; cygpath -w is required)"

adbq shell am force-stop "$PKG"
sleep 2
# WorldObject3D stage events are off by default (a live server should not spam this log); acceptance needs c=1/c=2, so turn them on first.
# The client reads the debug.samp.cef.worldobj system property, not an environment variable — on Android there is no way to inject env into the app
# process, so getenv would leave it permanently off.
adbq shell setprop debug.samp.cef.worldobj 1
adbq shell am start -n "$PKG/.SplashActivity" >/dev/null 2>&1
sleep 14
adbq exec-out screencap -p > "$OUT/e2e-1-list.png"

# First row in the list -> right detail panel -> Enter Server -> confirm dialog
adbq shell input tap $TAP_ROW1
sleep 3
adbq shell input tap $TAP_ENTER_PANEL
sleep 3
adbq shell input tap $TAP_ENTER_DIALOG
SINCE=$(date +%Y-%m-%dT%H:%M:%S)
echo "  Tapped Enter Server; only counting log lines after $SINCE; waiting for GTA + connection (max 120s)"

# ------------------------------------------------------------- 4. Wait for server events
say "4/5 Waiting for handshake and browser creation"
# Only take lines after SINCE: this excludes leftover connections from the previous round and does not rely on print flush order
# (open.mp script output is buffered, so line order is not time order and splitting on a marker line is unreliable).
slice() {
    awk -v s="$SINCE" '{
        if (match($0, /\[[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}/)) {
            ts = substr($0, RSTART + 1, 19);
            if (ts >= s) print;
        }
    }' "$LOG"
}
WAIT_OK=0
for _ in $(seq 1 60); do
    if slice | grep -q "PAGE -> SERVER"; then WAIT_OK=1; break; fi
    sleep 2
done
sleep 3
adbq exec-out screencap -p > "$OUT/e2e-2-ingame.png"

# WorldObject3D has to wait until the player really spawns. After connecting, this client (0.3.7-R5 mode) stops at the character-selection
# camera, with a Spawn button at the bottom; the server-side SpawnPlayer does nothing, so it must be tapped.
#
# Note this fullscreen overlay (browser 7) is **deliberately left open** here: it was created with focused=false,
# as a HUD-semantics page that by design must not consume touches. So "tapping Spawn still works while the fullscreen web page is up" is itself
# an assertion — using web instead of textdraw must not smother the virtual joystick / game buttons.
# World2D server entries do not report projections by default (normal players should not pay that cost); turn it on explicitly before acceptance.
adbq shell am broadcast -a com.sampmobile.CEFDEBUG -p "$PKG" --es action world2dprobe --ei on 1 >/dev/null 2>&1 || true
sleep 3
adbq shell input tap 1194 1008
SPAWN_OK=0
for _ in $(seq 1 30); do
    if slice | grep -q "board object id="; then SPAWN_OK=1; break; fi
    sleep 2
done
# Close that HUD after spawn: the board and floating pages sit under it, and leaving it would block the screenshot evidence.
adbq shell am broadcast -a com.sampmobile.CEFDEBUG -p "$PKG" --es action destroy --ei id 7 >/dev/null 2>&1 || true
# Give the client a moment to capture a frame / upload the texture, then screenshot to see whether the board shows the page content.
sleep 4
adbq exec-out screencap -p > "$OUT/e2e-3-world3d.png"

# World2D: the server sends CreateWorld2DBrowser at spawn, then calls
# SetWorld2DBrowserPos 8 seconds later to raise the page 1.5 m. Wait for the client to report "screen y changed before vs after the move",
# which proves the projection really follows world coordinates instead of being drawn at a fixed position.
SHIFT_OK=0
for _ in $(seq 1 15); do
    if slice | grep -q "WORLD2D SHIFT"; then SHIFT_OK=1; break; fi
    sleep 2
done
sleep 2
adbq exec-out screencap -p > "$OUT/e2e-4-world2d.png"

# --------------------------------------------------------------- 5. Assertions and summary
say "5/5 Assertions (only log lines after $SINCE)"
FAILED=0
check() {  # check <pattern> <description>
    if slice | grep -qE "$1" 2>/dev/null; then
        echo "  PASS  $2"
    else
        echo "  FAIL  $2"
        FAILED=$((FAILED + 1))
    fi
}
check "\[CEF\] RequestJoin pid=[0-9]+ from=" "Stock plugin received this round's RequestJoin from the client (including the IP check line)"
check "\[e2e\] OnCefInitialize player=[0-9]+ success=1 reason=0" "Handshake succeeded (CEF_INIT_OK; version/key/IP all match)"
check "\[e2e\] CEF_CreateBrowser" "Server called CEF_CreateBrowser after OnCefInitialize"
check "\[e2e\] OnCefBrowserCreated .*success=1" "Browser was created on the client (BrowserCreateResult returned to Pawn)"
check "\[e2e\] PAGE -> SERVER" "Page window.cef.emit reached the server (CEF_RegisterEvent fired the callback)"
check "\[e2e\] board object id=[0-9]+" "Fullscreen web HUD (focused=false) does not block game input: tapping Spawn works and the server got the real objectid"
check "\[e2e\] CEF_CreateWorldBrowser" "WorldObject3D: CEF_CreateWorldBrowser was sent"
check "\[e2e\] CEF_AttachBrowserToObject" "WorldObject3D: CEF_AttachBrowserToObject was sent"
check "CLIENT worldobj debug: .* c=1" "Client applied the page texture to the real objectid (stage=1 texture replaced)"
check "CLIENT worldobj debug: .* c=2" "First page frame reached the RenderWare texture (stage=2 first frame uploaded)"
check "\[e2e\] CEF_CreateWorld2DBrowser" "World2D: CEF_CreateWorld2DBrowser was sent"
check "CLIENT world2d debug: .* w=512 h=256 on=1" "Client placed the server-provided 512x256 page on screen (size comes from the protocol fields)"
check "\[e2e\] CEF_SetWorld2DBrowserPos" "World2D: CEF_SetWorld2DBrowserPos was sent"
check "\[e2e\] WORLD2D SHIFT ok" "Page moved with the world coordinates (projection recalculated every frame, coordinates changed)"
check "CLIENT world2d debug: browser=11 " "Second World2D exists independently and reports its own projection (multiple webs concurrently)"

# Latency probe: the server pings every 100ms, the page replies pong immediately, and the server computes the round trip from its own
# millisecond timestamp. Only the window average is gated here — tail spikes (GC/dropped frames) are invisible at the HUD refresh rate
# of 5~10Hz in this scenario, and gating p99 would make the assertion flap with machine state.
PING_MAX_AVG=$(slice | grep -oE "PING browser=9 .*avg=[0-9]+" | sed 's/.*avg=//' | sort -n | tail -1)
if [ -n "${PING_MAX_AVG:-}" ] && [ "$PING_MAX_AVG" -lt 100 ]; then
    echo "  PASS  Round-trip latency (server -> page -> server) window average max ${PING_MAX_AVG}ms < 100ms"
else
    echo "  FAIL  Round-trip latency window average max ${PING_MAX_AVG:-no data}ms (threshold 100ms)"
    FAILED=$((FAILED + 1))
fi

echo
echo "  Log excerpt for this round:"
slice | grep -E "\[e2e\]|RequestJoin" | sed 's/^/    /'
echo
echo "  Log: $LOG"
echo "  Screenshots: $OUT/e2e-1-list.png  e2e-2-ingame.png  e2e-3-world3d.png  e2e-4-world2d.png"
# After acceptance, turn the debug switch back off; do not leave the device in "spamming debug events" state.
adbq shell setprop debug.samp.cef.worldobj 0 >/dev/null 2>&1 || true
if [ $FAILED -eq 0 ] && [ $WAIT_OK -eq 1 ]; then
    echo
    echo "=== E2E (REAL PLUGIN) OK"
    exit 0
fi
echo
echo "=== E2E (REAL PLUGIN) FAILED ($FAILED assertion(s) failed)"
[ $WAIT_OK -eq 1 ] || echo "  Hint: no PAGE -> SERVER for this round within 120s. Check whether the phone really joined the server and whether the test-page HTTP is reachable."
exit 1
