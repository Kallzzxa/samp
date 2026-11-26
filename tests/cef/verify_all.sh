#!/usr/bin/env bash
# Run the full acceptance suite for the omp-cef Android client with a single command.
#
#   bash tests/cef/verify_all.sh [package]
#
# Four stages:
#   1. Offline logic assertions (javac/java, no device, no gradle)
#   2. Build the debug APK (the C++ constexpr assertions run here too)
#   2b. JNI symbol export check (a native method missing extern "C" is caught here on the spot)
#   3. Install + auto-enter the game (launcher -> single-player mode)
#   4. Full on-device regression (protocol / touch / IME / keys / World2D / WorldObject3D, with screenshots)
#
# Fully unattended: nothing needs tapping on the phone (if install is blocked by the system, "Continue install" is tapped automatically).
# Prints a PASS/FAIL summary at the end; logs land in tests/cef/out/.
#
# Overridable with environment variables:
#   JAVA_HOME         JDK path (falls back to the current environment when unset)
#   LAUNCH_TAP1/2     Coordinates of the launcher's "Single-player mode" and confirm buttons (defaults assume 2388x1080)
#   BOOT_WAIT         Extra seconds to wait after entering the game (default 35; GTA loads slowly)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/out"
PKG="${1:-com.sampmobile.launcher}"

JAVA_HOME="${JAVA_HOME:-}"
LAUNCH_TAP1="${LAUNCH_TAP1:-186 838}"     # Launcher home screen -> single-player mode
LAUNCH_TAP2="${LAUNCH_TAP2:-1783 690}"    # Copyright/notice page -> confirm
BOOT_WAIT="${BOOT_WAIT:-35}"
INSTALL_OK_TAP="${INSTALL_OK_TAP:-558 2215}"

mkdir -p "$OUT"
say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
adbq() { MSYS_NO_PATHCONV=1 adb "$@"; }
top_activity() {
    adbq shell dumpsys activity activities 2>/dev/null \
        | grep -m1 -o "topResumedActivity[^}]*" | head -1
}

FAILED=0
LOGIC_OK=0
BUILD_OK=0
JNI_OK=0
DEVICE_OK=0
REGRESSION_OK=0

# --------------------------------------------------------------- 0. Preflight checks
say "0/4 Checking device"
if ! adbq devices | grep -q "device$"; then
    echo "FAIL  No adb device connected"
    exit 2
fi
adbq shell pm path "$PKG" >/dev/null 2>&1 \
    && echo "  $PKG is installed (will be installed over it)" \
    || echo "  $PKG is not installed (fresh install)"

# --------------------------------------------------------- 1. Offline logic assertions
say "1/4 Offline logic assertions (no device needed)"
if bash "$HERE/logic_tests/run_logic_tests.sh" > "$OUT/verify-logic.log" 2>&1; then
    LOGIC_OK=1
    grep -E "^[0-9]+ passed|LOGIC TESTS OK" "$OUT/verify-logic.log"
else
    echo "FAIL  Offline logic assertions failed:"
    grep "FAIL" "$OUT/verify-logic.log" | head -10
    FAILED=$((FAILED + 1))
fi

# ------------------------------------------------------------------ 2. Build
say "2/4 Building debug APK (C++ constexpr assertions run here)"
( cd "$ROOT" && JAVA_HOME="$JAVA_HOME" cmd //c "gradlew.bat assembleDebug --offline" ) \
    > "$OUT/verify-build.log" 2>&1
if grep -q "BUILD SUCCESSFUL" "$OUT/verify-build.log"; then
    BUILD_OK=1
    grep "BUILD SUCCESSFUL" "$OUT/verify-build.log"
    APK="$ROOT/app/build/outputs/apk/debug/samp.apk"
    echo "  apk $(stat -c %s "$APK" 2>/dev/null || echo '?') bytes, sha256 $(sha256sum "$APK" | cut -c1-16)"
else
    echo "FAIL  Build failed:"
    grep -E "error:|FAILED" "$OUT/verify-build.log" | head -10
    FAILED=$((FAILED + 1))
fi

if [ $BUILD_OK -eq 1 ]; then
    # ------------------------------------------------ 2b. JNI symbols (anti-mangle)
    say "2b JNI symbol export check"
    if bash "$HERE/jni_symbol_check.sh" | tee "$OUT/verify-jni.log"; then
        JNI_OK=1
    else
        echo "FAIL  CefManager native methods have no resolvable symbol in the .so (see above)"
        FAILED=$((FAILED + 1))
    fi

    # ------------------------------------------------------- 3. Install + enter the game
    say "3/4 Installing and entering the game"
    # Note: adbq disables MSYS path conversion, so convert to a Windows path yourself;
    # otherwise adb receiving /d/server/... reports "failed to stat".
    adbq install -r "$(cygpath -w "$APK")" 2>&1 | tail -2

    # Some ROMs block install behind a "Continue install?" dialog.
    sleep 3
    if top_activity | grep -q "PackageInstaller"; then
        echo "  Install was blocked by the system; tapping 'Continue install' $INSTALL_OK_TAP"
        adbq shell input tap $INSTALL_OK_TAP
        sleep 6
    fi

    adbq shell am start -n "$PKG/.SplashActivity" >/dev/null 2>&1
    sleep 14
    adbq shell input tap $LAUNCH_TAP1
    sleep 3
    adbq shell input tap $LAUNCH_TAP2
    echo "  Waiting for GTA to load ${BOOT_WAIT}s ..."
    sleep "$BOOT_WAIT"

    # Confirm we really entered the game Activity; CefManager.install() only runs in-game,
    # and outside the game every CEFDEBUG broadcast is silently dropped.
    IN_GAME=0
    for _ in 1 2 3 4 5 6; do
        if top_activity | grep -q "com.sampmobile.game.SAMP"; then IN_GAME=1; break; fi
        echo "  Not in the game yet; waiting 10s more ..."
        sleep 10
    done
    if [ $IN_GAME -eq 0 ]; then
        echo "FAIL  Could not enter the game Activity ($(top_activity))"
        echo "      Enter the game screen manually, then re-run just section 4/4 of this script."
        FAILED=$((FAILED + 1))
    else
        DEVICE_OK=1
        echo "  In the game now"
    fi
fi

# ------------------------------------------------------- 4. Full on-device regression
if [ $DEVICE_OK -eq 1 ]; then
    say "4/4 Full on-device regression (protocol / touch / IME / keys / World2D)"
    bash "$HERE/run_browser_test.sh" "$PKG" > "$OUT/verify-device.log" 2>&1
    DEVICE_RC=$?
    grep -E "PASS |FAIL |STAGE|probe failure" "$OUT/verify-device.log" | tail -45
    if [ $DEVICE_RC -eq 0 ] && ! grep -q "FAIL " "$OUT/verify-device.log"; then
        REGRESSION_OK=1
    else
        FAILED=$((FAILED + 1))
    fi
fi

# ------------------------------------------------------------------ Summary
say "Acceptance summary"
[ $LOGIC_OK -eq 1 ]      && echo "  PASS  1. Offline logic assertions (142 items)"        || echo "  FAIL  1. Offline logic assertions"
[ $BUILD_OK -eq 1 ]      && echo "  PASS  2. Build (incl. C++ static assertions)"      || echo "  FAIL  2. Build"
[ $JNI_OK -eq 1 ]        && echo "  PASS  2b. JNI symbol export (anti C++ mangle)" || echo "  FAIL  2b. JNI symbol export"
[ $DEVICE_OK -eq 1 ]     && echo "  PASS  3. Install and enter the game"              || echo "  FAIL  3. Install and enter the game"
[ $REGRESSION_OK -eq 1 ] && echo "  PASS  4. Full on-device regression"              || echo "  FAIL  4. Full on-device regression"
echo
echo "  Logs: $OUT/verify-logic.log  verify-build.log  verify-device.log"
echo "  Screenshots: $OUT/1-created.png 1b-ime.png 1c-typed.png 2-hidden.png 3-visible.png 5-world2d.png"

if [ $FAILED -eq 0 ]; then
    echo
    echo "=== VERIFY ALL OK"
    exit 0
fi
echo
echo "=== VERIFY ALL FAILED ($FAILED section(s))"
exit 1
