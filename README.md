# SA-MP Mobile

面向 **GTA: San Andreas Android v2.10** 的 Android 原生 SA-MP 联机客户端（Java 启动器 + C++ 原生库 `libSAMPMOBILE.so`），支持 `arm64-v8a` 与 `armeabi-v7a` 双架构构建。

> 版权、第三方组件与合规声明请参阅 [NOTICE](NOTICE)。

---

## 项目简介

本项目基于开源的 Alyn SA-MP Mobile 架构持续演进，完成了 Android 启动器的重写和设计、大量底层崩溃与现代系统兼容性修复、稳定性加固以及多模组加载支持（五合一模组生态），并已通过实机联机验证。如在使用中发现问题，欢迎提交 Issue 反馈。

本项目不定期维护更新，坚持完全开放与透明，不含任何隐藏逻辑或私有功能；仓库内不包含任何游戏资源与商业字体，运行时文件均由用户从个人游戏拷贝中自行提取。欢迎自由构建、修改与 Fork，衍生版本需遵循相同的 **GNU GPL v3.0** 协议开源。

---

## 资源准备

出于版权合规要求，本仓库仅提供源码与开源依赖。从零构建并运行客户端前，请先准备以下外部资源：

| 资源 | 说明 | 是否必需 | 放置 / 使用方式 |
|---|---|---|---|
| **GTA:SA Android v2.10 安装包** | 提取 `libGTASA.so`、`libImmEmulatorJ.so` 及轻量 APK 资产（支持标准 ~26 MB APK、Split APK 及 2 GB+ 整合 APK，脚本会自动跳过大资源） | 编译 APK 必需 | 安装到手机后执行 `prepare_gtasa.py --from-adb`（推荐），或放进 `dist/` 用 `--apk` 提取 |
| **GTA:SA Android v2.10 游戏数据** | 提供 ~2.4 GB 单机基础地图、模型、动画与贴图（普通 ~26 MB APK 不含此数据，需配 `.obb`；`.xapk`/`.apks` 先解压） | 构建数据包必需 | 将 `main`/`patch` `.obb`（或 2 GB+ 完整 `.apk`）直接放入根目录 `dist/`，脚本按 `main` → `patch` 自动识别 |
| **PC 版 GTA:SA + SA-MP 0.3.7** | 提供 `SAMP.img`、`SAMP.ide`、`SAMPCOL.img` 与 PC TextDraw 贴图（无需第三方手机整合包） | 构建数据包必需 | 执行 `build_gamedata.py` 时通过 `--pc-samp` 指定本机安装目录 |
| **开源中文字体**（如 Noto Sans SC） | 生成 `SAMP/fonts/chinese.ttf` 以支持中文显示 | 可选（中文服推荐） | 放入 `dist/` 并命名为含 `font` 的字体文件（支持 `.ttf` / `.ttc` / `.otf`），省略则回退为自带 Roboto |

> **关于 GTA:SA Android v2.10 的说明：**
> - Google Play 现已更新至 v2.11+，底层内存布局与本客户端不兼容，请务必使用 **v2.10**。
> - `tools/prepare_gtasa.py` 会自动识别并剥离常见 v2.10 补丁包中的 Fastman92 注入（`libFastman92pluginLoader.so`），还原原版 `JNI_OnLoad`/`JNI_OnUnload`。
> - 若安装包仅含 64 位 `arm64-v8a`（缺少 32 位 `armeabi-v7a`），现代 64 位手机仍可直接编译运行，构建校验仅作提示不会拦截。

---

## 编译环境

| 工具 / 组件 | 版本要求 | 说明 |
|---|---|---|
| **JDK** | **17 – 21**（推荐 JDK 21） | Gradle 8.13 不支持 JDK 25+（会报 `major version 69`） |
| **Android SDK** | `compileSdk 35` / `targetSdk 35` / `minSdk 24` | 需安装 Platform 35 与 Build-Tools 35 |
| **Android NDK** | **`25.1.8937393`** | 已在 `app/build.gradle` 中固定 |
| **CMake** | **`3.22.1`**（或兼容版本） | 可通过 Android Studio SDK Manager 安装 |
| **Gradle** | **8.13**（仓库自带 Wrapper） | 无需全局安装 |
| **Python 3** | 3.10+ | 运行 `tools/` 脚本；`build_gamedata.py` 需安装 `numpy` |

---

## 构建与部署流程

### 1. 克隆仓库并配置 `local.properties`

```bash
git clone https://github.com/XiaoNiaoa/samp-mobile.git
cd samp-mobile
cp local.properties.example local.properties
```

在 `local.properties` 中填入本机 Android SDK 路径（用 Android Studio 打开工程会自动生成）：

```properties
# Windows
sdk.dir=C\:\\Users\\<YourUser>\\AppData\\Local\\Android\\Sdk

# macOS / Linux
# sdk.dir=/Users/<YourUser>/Library/Android/sdk
```

### 2. 提取 GTA:SA v2.10 运行时文件

先在手机上安装好 GTA:SA v2.10 APK（无需导入单机数据包，`data`/`obb` 为空即可），再运行脚本提取（目标路径已加入 `.gitignore`，每次提取自动清理旧资产并自动净化 Fastman92 注入）：

```bash
# 方式 A（推荐）：连接 ADB 直接从手机提取
python tools/prepare_gtasa.py --from-adb

# 方式 B：从本地 APK 或 Split-APK 目录提取
python tools/prepare_gtasa.py --apk dist/gtasa_2.10.apk

# 清理本地提取的专有文件
python tools/prepare_gtasa.py --clean
```

### 3. 编译 APK

Debug 包无需签名配置或 `google-services.json`，产物输出至 `app/build/outputs/apk/debug/samp.apk`：

```powershell
# Windows (PowerShell) — 若默认 JDK >= 25，先指定 JDK 17–21
# $env:JAVA_HOME = "C:\Path\To\jdk-21"
.\gradlew.bat assembleDebug
```

```cmd
:: Windows (CMD)
set JAVA_HOME=C:\Path\To\jdk-21
gradlew.bat assembleDebug
```

```bash
# macOS / Linux
./gradlew assembleDebug
```

> 仅做源码/C++ 干跑编译检查（未提取 GTA:SA 文件）时可加 `-PskipGtasaCheck=true`。
>
> - **Release 签名（可选）**：在 `local.properties` 或环境变量中配置 `SAMP_STORE_FILE`、`SAMP_STORE_PASSWORD`、`SAMP_KEY_ALIAS`、`SAMP_KEY_PASSWORD` 后执行 `./gradlew assembleRelease`（切勿提交密钥文件）。
> - **Firebase Crashlytics（可选）**：参考 `app/google-services.json.example` 放置 `app/google-services.json` 即可自动启用，默认关闭。

### 4. 生成数据包（`.zip`）并在启动器一键导入

1. 将 GTA:SA v2.10 的 `main`/`patch` `.obb`（或 2 GB+ 完整 `.apk`）放入仓库根目录 `dist/`；
2. （可选）将中文字体（如 `font.ttf` / `font.ttc` / `font.otf`）放入 `dist/`；
3. 指定已安装 **SA-MP 0.3.7** 的 PC 版 GTA:SA 目录，执行一条命令即可自动生成 `dist/gamedata.zip`：

```bash
python tools/build_gamedata.py --pc-samp "D:\Program Files (x86)\Rockstar Games\GTA San Andreas"
```

> 可选参数：`--gtasa`（手动指定 OBB/APK 路径）、`--cjk-font`（手动指定字体）、`--out`（默认 `dist/gamedata.zip`，传目录路径则输出散文件夹）、`--keep-audio`（保留完整音频）、`--scm-dir`（替换脚本）。

**手机端一键导入：**
1. 将 `dist/gamedata.zip` 复制到手机任意目录（如 `Download/`），无需手动操作 `Android/data/`；
2. 安装并打开 `SA-MP Mobile`，在 **游戏设置 → 联机设置 → 一键导入 .zip**（或点击进服时按弹窗提示）选择该文件；
3. 启动器会自动流式解压至自身 `files/` 目录，同时保留已有 `settings.json` 与服务器列表，彻底规避 Android 14 目录权限与层级错位问题。

---

## 本地自动校验与测试

不依赖真机或模拟器即可执行以下验证：

- **双 ABI 编译期布局守卫**：`app/src/main/cpp/SAMPMOBILE/AbiGuards.cpp` 在编译 `arm64-v8a` / `armeabi-v7a` 时校验指针宽度、RakNet 类型、SA-MP 同步包大小及 GTA:SA 核心引擎结构体布局。
- **JUnit 单元测试**：`./gradlew testDebugUnitTest`
- **跨平台静态与产物自检**（仅依赖 Python 3 标准库）：

  ```bash
  # 校验开源仓库状态
  python tools/selfcheck.py

  # 校验完整提取与双 ABI 打包产物
  python tools/selfcheck.py --require-gtasa --require-apk
  ```

---

## 常见问题

- **报错 `Unsupported class file major version 69`**：当前 `JAVA_HOME` 为 JDK 25+，请切换至 JDK 17–21 后重新构建。
- **Android 14+ 加载 AML / MonetLoader 异常**：请保留 `app/build.gradle` 中的 `useLegacyPackaging = true` 与 `AndroidManifest.xml` 中的 `android:allowNativeHeapPointerTagging="false"`。
- **首次拉取 Gradle 依赖较慢**：`settings.gradle` 已在官方源后配置镜像回退。

---

## 许可证与免责声明

本项目原始及修改代码采用 **GNU General Public License v3.0** 开源发布，协议全文见 [LICENSE](LICENSE)，第三方组件许可证清单见 [third_party/licenses/THIRD_PARTY_NOTICES.md](third_party/licenses/THIRD_PARTY_NOTICES.md)。

本项目基于开源的 Alyn SA-MP Mobile 架构继续开发，保留原有文件归属声明。本项目与 Rockstar Games、Take-Two Interactive 或 SA-MP 官方团队无任何关联，不分发任何 Rockstar 专有二进制、游戏数据包或商业字体，详情见 [NOTICE](NOTICE)。
