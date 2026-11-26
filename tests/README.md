# tests/

The unified automated checks and tests directory for SA-MP Mobile.

Design principle: **only verify 100% objectively verifiable engineering, binary, symbol, and Android platform contracts, plus executable tests that share source with the real product code** — never hard-code guesses about business code that goes stale as the design evolves into a gate.

---

## Unified entry point: `tests/check.py`

```powershell
# Daily quick check (~2–3 s): compliance & hygiene + platform baseline + NDK binary / 537 symbols / real JNI + SDK APK tool checks
python tests/check.py

# Full check: all of the above + C++ desktop unit tests + Gradle JVM unit tests + gamedata chunk reassembly round-trip
python tests/check.py --full

# Pre-release strict gate: --full + requires extracted GTA:SA and a built APK + Android Lint static checks
python tests/check.py --release

# Run a single stage alone
python tests/check.py --stage <compliance|platform|abi|apk|native|jvm|gamedata|lint>
```

---

## 5-stage check flow

| Stage | Module | What is checked & official toolchain |
|---|---|---|
| **Stage 1** | `checks/compliance.py` | Legal/license files, no commercial fonts in `res/font/`, `.gitignore` blocking of proprietary assets, no hardcoded secrets / local paths / device serial numbers, `gradlew` LF line endings |
| **Stage 2** | `checks/platform.py` | `app/build.gradle` dual ABI / pinned NDK / `useLegacyPackaging` / conditional Firebase; `AndroidManifest.xml` TBI disabled and the `:game` standalone process; `monetloader_lib.zip` and the GTA:SA extracted-asset index |
| **Stage 3** | `checks/binary_abi.py` | Dual-ABI `jniLibs/*.so` ELF headers; NDK `llvm-nm` full 537-symbol export check of `Symbols.def` for both ABIs; NDK `llvm-readelf` checks on the built `libSAMPMOBILE.so` / `libSCAnd.so`: **zero `TEXTREL`**, **`DT_NEEDED` dependency closure** (links only NDK public libraries or bundled libraries), **Android 15+ 16 KB page-alignment hints**, and dual-ABI **real ELF exported symbols ↔ Java `native` method alignment** |
| **Stage 4** | `checks/apk_verify.py` | APK ZIP structure and dual-ABI library completeness; Android SDK **`aapt2`** cross-checks the compiled binary `AndroidManifest.xml` (package name, `minSdk`/`targetSdk`, dual ABI, `extractNativeLibs`, `allowNativeHeapPointerTagging`); **`zipalign -c 4`** alignment check; **`apksigner verify`** signature validity check |
| **Stage 5** | `checks/runners.py` & `checks/gamedata.py` | ① 20 C++ desktop unit tests in `tests/native/` (including cross-checks against open.mp upstream); ② `gradlew testDebugUnitTest` Java JUnit unit tests; ③ `publish_gamedata.py` chunk / manifest / reinstall SHA-256 end-to-end round-trip; ④ (`--lint` / `--release`) official Android Lint (`gradlew lintDebug`) |

---

## Directory Layout

| Path | Responsibility |
|---|---|
| `tests/check.py` | Unified command-line check entry point |
| `tests/checks/` | Modular objective gate implementations (Python 3 standard library only + local Android SDK/NDK tools) |
| `tests/native/` | Desktop C++ unit tests (CMake + Ninja + CTest; run with `sh tests/native/run.sh` under WSL/Linux, not shipped in the APK):<br>- `proto/`: RakNet BitStream, wire format, `/netcap`, inbound RPC / sync / outbound packet unit tests<br>- `core/`: engine-free pure-logic unit tests for client core (`InputArbiterCore`, `SettingsContract`)<br>- `interop/`: cross-check tests against the local `reference/open.mp upstream source` (auto-skipped when the local source is absent) |
| `tests/cef/` | `omp-cef` protocol KAT vectors, logic assertions, and on-device browser regression suite (see `tests/cef/verify_all.sh`) |
| `app/src/test/` | Standard Gradle JVM unit tests (e.g. `ClientSettingsTest`, `ModManagerTest`), invoked together by `tests/check.py --full` |
