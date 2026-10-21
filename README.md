# rtg-mobile · 节奏天国移动版

《节奏天国》的实验性 **Android arm64 原生兼容层**。离线改写游戏的 AArch64 模块，由宿主提供系统服务，将 NVN 图形调用转换为 Vulkan，以 AAudio 输出音频，并提供触控与震动。

本仓库发布兼容层和开发工具源码，**不提供游戏本体、解密密钥、字体、存档、游戏着色器缓存或包含这些内容的 APK**。运行需要自行准备有权使用的对应游戏文件。项目与任天堂无隶属关系。

## 当前状态

- Android 10 / API 29 及以上，`arm64-v8a`，图形路径使用 Vulkan 1.1。
- 游戏内容以 60 Hz 为基准；优先请求同分辨率 120/60 Hz 显示模式，**不代表游戏以 120 FPS 运行**。
- 触控采用 Touchable 的 rhythm 布局：右半屏 A，左侧按键随当前关卡自动切换为单击、左右两键或四向区；左下小十字与侧边栏 B、−、+，Y/X 按关卡显示，支持多点触控与透明度设置。
- 按键使用自绘液态玻璃样式；画面保持原比例居中，左右/上下黑边由 Vulkan 呈现路径绘制整幅画面的模糊环境背景（64×36 光照图、跨帧平滑、外缘羽化）。
- 已有音频混音、手柄图标字体补全、存档路径；震动按 Switch 左右马达的低/高频段能量合并为强度与锐度，Android 以 256 级振幅和锐度包络输出；失焦及切后台时释放输入。
- 默认使用游戏的高分辨率渲染档：主画面 1920×1080，开始菜单文字层 1000×900，去掉原来的 720p → 1080p 放大；掌机输入模式保持不变。高分辨率会增加 GPU 开销，不能保证所有场景达到 60 FPS。
- 真机兼容性仍在验证。已测关卡片段可接近 60 FPS；华为 ALN-AL80 本轮短测中，高清开始菜单约 39 FPS，同构建低清档约 51 FPS。场景切换仍可能有长帧；不保证全关卡稳定帧率、所有 GPU 正确显示或固定功耗。
- 小米 8（LineageOS Android 15、Adreno 630 驱动 `0x801f6000`）已针对菜单合成闪烁加入兼容处理；实机验证覆盖启动、开始菜单、选关和首关教程，尚未覆盖全部关卡。
- 当前是开发版：应用启用 `debuggable`，没有面向最终用户的游戏资源导入向导。不提供 iOS/macOS 运行目标。

包名：`org.rhythmtengoku.mobile`；原生库：`librtg_mobile_android.so`。

## 目录

| 路径 | 内容 |
| --- | --- |
| `rtg/android/` | Android 外壳、JNI、音频、输入、自适应触控提示、震动、线程与文件服务 |
| `rtg/host/` | 模块装载、AArch64 调用门和系统服务兼容层 |
| `rtg/gfx/nvn/` | NVN 接口、命令记录、Vulkan 后端与黑边光效 |
| `rtg/gfx/tex/` | Tegra 纹理解排布、BC 解码与预解码缓存 |
| `rtg/gfx/tools/dmsh_xlate/` | 基于 Ryujinx 的离线着色器翻译器 |
| `rtg/third_party/` | 保留原许可证的第三方源码 |
| `tools/` | 模块改写、字体生成、打包、检查与设备测量 |
| `game/`、`build/`、`run/`、`.local/` | 仅供本地使用，全部被 Git 忽略 |

部分底层接口仍沿用上游的 `dm_`、`mtl_` 命名，以减少适配层改动；Android 实际渲染后端是 Vulkan。

## 无需游戏文件的构建与测试

需要 Python 3.9+、C11 编译器、JDK 17、CMake 3.22+。Android 原生构建使用 NDK `27.1.12297006`；打包另需 SDK Platform 34、Build Tools 35.0.0、Platform Tools。

```sh
# 已安装的 Android SDK / NDK 路径；按本机设置。
export ANDROID_SDK_ROOT="$HOME/Android/Sdk"
export ANDROID_NDK_HOME="$ANDROID_SDK_ROOT/ndk/27.1.12297006"
export JAVA_HOME="/path/to/jdk-17"

cmake -S rtg/android -B build/android \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29
cmake --build build/android --parallel
python3 tools/test_android_hid_bindings.py --native build/android/librtg_mobile_android.so

# Python、C 和纯 JVM 回归，无需手机、SDK 或原版资源。
python3 tools/run_checks.py
# 可选：对 C 回归启用 AddressSanitizer / UBSan。
python3 tools/run_checks.py --sanitize --output build/checks-sanitized
```

macOS 上请将 `JAVA_HOME` 设置为实际 JDK 目录，例如通过 `/usr/libexec/java_home -v 17` 获取；如果 `javac` 是符号链接，也应使用真实 JDK 路径。SDK 可通过 Android Studio 或 `sdkmanager` 安装。

Linux 上另有线程、文件恢复与翻译队列测试：

```sh
python3 rtg/android/tests/run_merge_checks.py --output build/linux-host
python3 rtg/android/tests/run_translation_checks.py --output build/linux-queue
```

CI 对公开源码执行扫描、CPU/JVM 测试和 Android 原生构建，不使用或上传游戏资源。这些检查不能替代真机的 GPU、声音、输入延迟和生命周期测试。

## 本地准备游戏模块

本项目针对已适配的游戏构建；正式版 `main` 的构建号前 16 字节为 `03ff4a3bec96bde3a2e37ce3aa4caa0c`。其他构建未经验证。改写器对已登记位置检查原始字节，不能通过跳过断言来适配版本。

将自己的已提取文件放在以下位置，或在后续命令中传入仓库外路径：

```text
game/extracted/ExeFS/main
game/extracted/ExeFS/sdk
game/extracted/RomFS/...
game/extracted/Control/control.nacp
game/extracted/Control/icon_SimplifiedChinese.dat
```

仓库不提供游戏提取工具、密钥或下载渠道。

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements.txt -r tools/requirements-font.txt

DM_TARGET=android python tools/patch_nso_rtg.py game/extracted/ExeFS/main build/modules main
DM_TARGET=android python tools/patch_nso_rtg.py game/extracted/ExeFS/sdk build/modules sdk
```

打包仅使用每个模块的 `.patched.bin`、`.fixups.tsv` 和 `.gates.tsv`。这些文件包含或来源于游戏程序，即使能由脚本重新生成，也不能加入公开源码仓库。

## 本地准备着色器和字体

着色器工具需要 .NET SDK 10、SPIRV-Tools（`spirv-opt` / `spirv-val`）和 SPIRV-Cross。先从自己的运行环境取得成对的 `*_st0_ctl.bin` / `*_st0_data.bin`、`*_st1_ctl.bin` / `*_st1_data.bin`，放在 `run/shaderdump/`。`st0` 为顶点阶段，`st1` 为片段阶段。

**当前缺少从原始 RomFS 自动生成完整着色器输入的一键流程。** 仅克隆仓库可以构建兼容层和运行单测；若没有自行取得的着色器转储，尚不能直接得到可运行游戏的 APK。兼容层保留 `DM_SHADERDUMP` 诊断入口，但未覆盖的关卡仍可能需要补充转储与适配。

```sh
dotnet build rtg/gfx/tools/dmsh_xlate/dmsh_xlate.csproj -c Release

python tools/build_rtg_vulkan.py \
  --dump run/shaderdump --output build/shaders --work build/shader-work \
  --shader-tools /path/to/spirv-tools/bin \
  --spirv-cross /path/to/spirv-cross
```

黑边光效使用的内置 SPIR-V 已生成在 `rtg/gfx/nvn/nvn_vk_ambient_spv.h`，日常构建不需要着色器编译器。只有修改 `rtg/gfx/nvn/ambient/` 下的 GLSL 后才需要 `glslangValidator` 并重新生成：

```sh
python3 tools/build_ambient_shaders.py
```

字体必须由自己提供，具有 TrueType `glyf` 轮廓、2048 units/em，并覆盖所需语言及构建器要求的基础符号。构建器补充控制器图标，读取本地 `RomFS/mesg` 校验覆盖。字体及派生字体的使用、分发条件以原字体许可证为准，仓库不包含字体。

```sh
python tools/build_rtg_shared_font.py \
  --source /path/to/your-font.ttf --output build/fonts/ArialUnicode.ttf \
  --messages game/extracted/RomFS/mesg
python tools/test_rtg_controller_font.py \
  --font build/fonts/ArialUnicode.ttf --source /path/to/your-font.ttf \
  --messages game/extracted/RomFS/mesg
```

`ArialUnicode.ttf` 是当前运行时固定的资产文件名，不意味着仓库附带该商业字体。构建工具同时生成 `.json` 校验报告，打包时两者必须匹配。

## 本地打包与运行

打包直接读取上述模块，**不依赖历史 APK**。应用显示名称和启动图标读取自本地游戏的 Control 内容：`control.nacp` 和对应语言的 `icon_<语言>.dat`，默认使用简体中文，可用 `--game-language` 选择其他语言。名称不追加“移动版”，原版 JPEG 图标按原始字节复制；这些资源只生成在被忽略的构建目录中，不进入源码仓库。Control 内容需从自己的游戏中单独提取，不能用游戏程序的 RomFS 目录替代。

先创建仅供本地开发的签名密钥：

```sh
mkdir -p .local
export RTG_KEYSTORE_PASSWORD=android  # 公开的开发用口令，仅用于此调试密钥。
"$JAVA_HOME/bin/keytool" -genkeypair -keystore .local/debug.keystore \
  -storepass:env RTG_KEYSTORE_PASSWORD -keypass:env RTG_KEYSTORE_PASSWORD \
  -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
  -dname 'CN=Android Debug,O=Android,C=US'

python tools/package_android.py \
  --modules build/modules --native build/android/librtg_mobile_android.so \
  --shaders build/shaders --shared-font build/fonts/ArialUnicode.ttf \
  --messages game/extracted/RomFS/mesg \
  --game-control game/extracted/Control \
  --jdk "$JAVA_HOME" --sdk "$ANDROID_SDK_ROOT" \
  --keystore .local/debug.keystore --output build/local-apk
```

输出目录必须不存在。工具验证模块平台标记、字体及着色器哈希，编译 Java、对齐 APK，并验证签名；不会自动安装。口令经环境变量传给签名工具，不写入构建凭据。生成的 APK 含本地游戏模块、派生着色器及字体，**只用于本地测试，不作为源码开源产物发布**。

```sh
# 多设备时给每条 adb 命令加 -s <设备序列号>。
adb install -r build/local-apk/rhythm-tengoku-mobile.apk
adb shell mkdir -p /sdcard/Android/data/org.rhythmtengoku.mobile/files/rhythm-tengoku/romfs
adb push game/extracted/RomFS/. /sdcard/Android/data/org.rhythmtengoku.mobile/files/rhythm-tengoku/romfs/
adb shell am start -n org.rhythmtengoku.mobile/.GameActivity
```

存档位于应用私有目录 `files/rhythm-tengoku/save/`。更新应使用同一密钥；换签名导致无法覆盖安装时，先备份存档，不要直接卸载或清除应用数据。

开始菜单中的朗读设置在按 A 确认变更后，通过游戏原有保存流程提交；按 B 取消不保存。此修复位于模块改写器，已有本地模块需要重新运行 `patch_nso_rtg.py` 生成 `main` 后再打包。选择“不朗读”、按 A 并等到返回开始菜单后，可检查设置落盘和重启后的保留情况：

```sh
python3 tools/check_android_narration_persistence.py --serial <设备序列号> \
  --expected-mode 0 --restart --output run/narration-persistence
```

`0/1/2` 分别为“不朗读 / 朗读 / 尽情朗读”。该检查只验证已提交存档；重启后仍需进入朗读设置核对选项，并验证菜单实际声音。

设备测量入口：`tools/check_android_startup.py`、`tools/check_android_frame_pacing.py`、`tools/check_android_audio.py`。先查看各自 `--help`；这些命令会操作指定设备并将日志、截图或音频写入本地输出目录，结果不要直接提交。

文字清晰度回归：在游戏的**开始菜单**（包含“单人游玩”的列表）执行以下命令。它抓取一帧 Vulkan 诊断，验证文字层尺寸和最终复制没有再次放大；通过后仍需看截图并另外测量帧率、音频和生命周期。

```sh
python3 tools/check_android_ui_resolution.py --serial <设备序列号> --output run/ui-resolution
```

如需在同一个构建上对照原来的低清档，在应用私有 `files/rhythm-tengoku/diagnostics/` 创建 `handheld-render-profile` 文件并重新启动；删除文件、重启后恢复默认高清档。该开关只改变游戏渲染档，不改变 Android 的 CPU/GPU 调频策略。

黑边光效默认强度为 `0.8`。在同一诊断目录放入 `ambient-bars.txt`（内容为 `0`–`1` 的数字，`0` 关闭）可调整；超出范围或格式错误的文件会被忽略并记录。游戏画面本身不参与该效果的绘制。

## 贡献与公开提交

```sh
# 每次新克隆后启用本项目的提交检查。
git config core.hooksPath .githooks
python3 tools/check_public_tree.py
python3 tools/run_checks.py

# 暂存审查过的源码后，检查将实际提交的内容。
python3 tools/check_public_tree.py --staged
git diff --cached --stat
git diff --cached
```

`.gitignore` 排除本地文件；检查脚本另行拒绝二进制、游戏资源目录、常见凭据、个人绝对路径和异常大文件，并检查暂存区内容。它只能识别已有规则，提交前仍需审阅文本中的个人信息、令牌和游戏原始数据。不要对整个工作目录直接压缩发布，也不要用 `git add -f` 绕过排除规则。

报告问题请注明设备型号、Android 版本、GPU、源码版本和复现步骤；提供脱敏后的必要日志，避免上传游戏文件、存档、密钥及完整设备转储。修改系统服务或渲染时请附相应回归与真机验证结果，区分已经验证和仍待验证的部分。

## 来源与许可

本项目参考了 [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile) 的思路。源代码采用 [MIT License](LICENSE)，保留上游版权声明。第三方组件及移植算法另见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。游戏、字体及其派生资源不属于本仓库的开源授权范围。
