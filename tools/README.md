# tools/

项目构建准备、数据包打包与自动自检脚本目录。

| 脚本 | 用途 |
|---|---|
| `prepare_gtasa.py` | 构建前从已安装 GTA:SA v2.10 的 ADB 设备（`--from-adb`，推荐）或本地 APK/Split-APK（`--apk`）提取 `libGTASA.so`、`libImmEmulatorJ.so` 与轻量 APK 资产到本地（已 `.gitignore`），自动跳过大体积 OBB 目录并自动剥离 Fastman92 补丁注入，支持 `--clean` 一键清除 |
| `selfcheck.py` | 零设备跨平台自动自检脚本（Python 3 标准库），校验合规与许可证文件、构建/Manifest 底线、反回归红线、JNI 导出对齐、双 ABI `jniLibs` ELF 头、资产完整性、路径卫生与 APK 产物结构（可选 `--require-gtasa` / `--require-apk`） |
| `build_gamedata.py` | 仅需官方 GTA:SA Android 2.10（`--gtasa`）与官方 PC GTA:SA + SA-MP 0.3.7（`--pc-samp`），全自动拆分转换并打包为单个 `gamedata.zip`（可直接交由启动器一键导入，含带 Mipmap/大小写别名的 `texdb/samp/`、`samp_extra/`、开源字体与纯净空白 `main.scm`） |
