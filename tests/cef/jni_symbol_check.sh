#!/usr/bin/env bash
# Check that every native method declared in CefManager.java really has a matching symbol in the build output,
# and that it is an **unmangled** JNI name.
#
# Why this check exists: if a native method is missing extern "C", C++ mangles it into
# _Z66Java_..., and the Java-side call throws UnsatisfiedLinkError directly. If the Java side also swallows it with
# try/catch, the symptom is "code changed, compiled fine, behaviour completely unchanged", and on a machine where
# logcat is unreliable there is not even a trace to find. This exact pitfall was hit on 2026-11-23
# (nativeForgetWorldObject had no effect, so WorldObject3D entries were never released).
#
#   bash tests/cef/jni_symbol_check.sh
#
# Exit code 0 = all symbols present; 1 = some missing (printed one by one).
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
JAVA="$ROOT/app/src/main/java/com/sampmobile/game/ui/cef/CefManager.java"
PREFIX="Java_com_sampmobile_game_ui_cef_CefManager_"

# The llvm-nm host directory name varies by platform: CI is linux-x86_64, this Windows machine is
# windows-x86_64. It used to list Windows only, which silently SKIPPED on a Linux runner,
# showing as "CI green but no symbol was actually checked" — a false negative, more dangerous than a red light.
find_nm() {
    local sdk host
    for sdk in "${ANDROID_SDK_ROOT:-}" "${ANDROID_HOME:-}" /d/android-sdk "$HOME/android-sdk"; do
        [ -n "$sdk" ] || continue
        for host in linux-x86_64 darwin-x86_64 windows-x86_64; do
            for f in "$sdk"/ndk/*/toolchains/llvm/prebuilt/"$host"/bin/llvm-nm*; do
                [ -x "$f" ] && { printf '%s\n' "$f"; return 0; }
            done
        done
    done
    for f in llvm-nm nm; do
        command -v "$f" >/dev/null 2>&1 && { command -v "$f"; return 0; }
    done
    return 1
}

find_so() {
    local so
    so="$(ls -t "$ROOT"/app/build/intermediates/cxx/Debug/*/obj/arm64-v8a/libSAMPMOBILE.so 2>/dev/null | head -1)"
    if [ -z "$so" ]; then
        # AGP changed the intermediate-output layout; fall back to searching the whole directory once.
        so="$(find "$ROOT/app/build" -type f -name libSAMPMOBILE.so -path "*arm64-v8a*" 2>/dev/null | head -1)"
    fi
    printf '%s' "$so"
}

NM="$(find_nm)" || { echo "FAIL  llvm-nm not found (no ndk under ANDROID_SDK_ROOT=$ANDROID_SDK_ROOT)"; exit 1; }
SO="$(find_so)"
[ -n "$SO" ] || { echo "FAIL  libSAMPMOBILE.so not found; build first"; exit 1; }
[ -f "$JAVA" ] || { echo "FAIL  $JAVA not found"; exit 1; }

# Native method names: `private static native void nativeFoo(int id);`
NATIVES="$(grep -oE 'native +(void|int|long|boolean|String|byte\[\]|float|double) +native[A-Za-z0-9_]+ *\(' "$JAVA" \
    | sed -E 's/.* (native[A-Za-z0-9_]+) *\($/\1/' | sort -u)"
COUNT=$(printf '%s\n' "$NATIVES" | grep -c . || true)
[ "$COUNT" -gt 0 ] || { echo "FAIL  No native methods parsed from $JAVA (is the parsing rule broken?)"; exit 1; }

SYMFILE="$ROOT/tests/cef/out/.jni-symbols.txt"
mkdir -p "$(dirname "$SYMFILE")"
"$NM" -D --defined-only "$SO" | awk 'NF>=3 {print $3}' > "$SYMFILE"

MISSING=0
for n in $NATIVES; do
    # Must use a file, not a pipe: `grep -q` exits immediately on a hit, and the printf on the other end of the pipe then gets
    # SIGPIPE, which under set -o pipefail fails the whole pipeline — a 100% false positive.
    if grep -qx "$PREFIX$n" "$SYMFILE"; then
        continue
    fi
    MISSING=$((MISSING + 1))
    # Give the specific reason: mangled, or not implemented at all.
    if grep -q "^_Z[0-9]*$PREFIX$n" "$SYMFILE"; then
        echo "FAIL  $n: symbol mangled by C++ (missing extern \"C\")"
    else
        echo "FAIL  $n: no implementation at all"
    fi
done

if [ "$MISSING" -eq 0 ]; then
    echo "JNI SYMBOLS OK ($COUNT native methods in $(basename "$SO"))"
    exit 0
fi
echo "JNI SYMBOLS FAILED ($MISSING/$COUNT)"
exit 1
