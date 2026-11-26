#!/usr/bin/env bash
# Offline regression for the pure logic of the CEF input path (no phone / game / gradle / network needed).
#
#   bash tests/cef/logic_tests/run_logic_tests.sh
#
# Approach: copy CefKeys.java and CefLogic.java from the product source verbatim into a temporary ASCII directory
# (a Chinese path would confuse javac), use android.jar only as a **compile-time** classpath and **deliberately leave it out at runtime**
# — so if someone introduces an Android runtime type into either class,
# the test fails straight away with NoClassDefFoundError, flagging the broken "pure logic" constraint immediately.
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CEF_DIR="$ROOT/app/src/main/java/com/sampmobile/game/ui/cef"
TEST_SRC="$ROOT/tests/cef/logic_tests/LogicTests.java"

# --- find javac -----------------------------------------------------------------
JAVAC=""
JAVA=""
if [ -n "${JAVA_HOME:-}" ] && [ -x "$JAVA_HOME/bin/javac.exe" ]; then
    JAVAC="$JAVA_HOME/bin/javac.exe"
    JAVA="$JAVA_HOME/bin/java.exe"
elif [ -x "D:/server/DAYZSA/_build_env/jdk/jdk-21.0.2/bin/javac.exe" ]; then
    JAVAC="D:/server/DAYZSA/_build_env/jdk/jdk-21.0.2/bin/javac.exe"
    JAVA="D:/server/DAYZSA/_build_env/jdk/jdk-21.0.2/bin/java.exe"
elif command -v javac >/dev/null 2>&1; then
    JAVAC="$(command -v javac)"
    JAVA="$(command -v java)"
else
    echo "FAIL  javac not found (set JAVA_HOME)" >&2
    exit 2
fi

# --- find android.jar (take the highest platform) ----------------------------------------
ANDROID_JAR=""
SDK="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-D:/android-sdk}}"
for d in "$SDK"/platforms/android-*/; do
    [ -f "$d/android.jar" ] && ANDROID_JAR="$d/android.jar"
done
if [ -z "$ANDROID_JAR" ]; then
    echo "FAIL  android.jar not found under $SDK/platforms" >&2
    exit 2
fi

for f in "$CEF_DIR/CefKeys.java" "$CEF_DIR/CefLogic.java" "$TEST_SRC"; do
    if [ ! -f "$f" ]; then
        echo "FAIL  missing source: $f" >&2
        exit 2
    fi
done

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP" >/dev/null 2>&1 || true' EXIT
SRC="$TMP/src/com/sampmobile/game/ui/cef"
OUT="$TMP/out"
mkdir -p "$SRC" "$OUT"

cp "$CEF_DIR/CefKeys.java" "$CEF_DIR/CefLogic.java" "$TEST_SRC" "$SRC/"

echo "== compiling (android.jar is compile-time only)"
if ! "$JAVAC" -nowarn -encoding UTF-8 -cp "$ANDROID_JAR" -d "$OUT" "$SRC"/*.java; then
    echo "FAIL  compile error" >&2
    exit 1
fi

# Runtime classpath is only $OUT, no android.jar — see the header comment.
echo "== running (no android.jar on the runtime classpath)"
"$JAVA" -cp "$OUT" com.sampmobile.game.ui.cef.LogicTests
STATUS=$?
if [ $STATUS -ne 0 ]; then
    echo "LOGIC TESTS FAILED (exit $STATUS)" >&2
    exit $STATUS
fi
