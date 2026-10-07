# vibeled：USB 状态灯与 Agent Hook

`vibeled` 是原 `logled` 工具的 C++ / Qt 重写版。Windows 和 macOS 使用相同源码。无参数启动界面；带参数进入命令行。灯光命令通过接收器已有的 USB CDC 接口发送，不使用 Wi-Fi、UDP 或蓝牙。遥控器仍使用原有蓝牙连接。

## 一键打包与运行

`build/` 中的 EXE 是编译中间产物，不包含 Qt DLL，直接双击会提示缺少 DLL。请使用 `dist/` 下的完整发行包。

| 平台 | 仓库根目录的一键入口 | 输出 |
| --- | --- | --- |
| Windows | 双击 `package-vibeled.cmd` | `dist/vibeled-1.0.0-windows-x64.zip` |
| macOS | 双击 `package-vibeled.command` | `dist/vibeled-1.0.0-macos-<架构>.dmg` |

版本号自动读取 `tool/CMakeLists.txt`，macOS 架构从构建后的程序读取。每次构建先运行 Qt 和 Agent 测试，再在全新的临时目录收集依赖、打包并生成同名 `.sha256` 校验文件。失败时返回非零退出码并保留终端错误信息。

Windows：将 ZIP **完整解压**，再双击其中的 `vibeled/vibeled.exe`；不要直接在 ZIP 内启动，也不要只复制 EXE。包内包含 Qt DLL、插件和 MSVC 运行库，Windows 10/11 x64 上无需安装 Qt 或运行库。图形界面和命令行不需要 Python。包内 `hooks/` 提供三个兼容 Agent 入口，使用它们时需要 Python 3.10+。

macOS：打开 DMG，将 `vibeled.app` 拖到 `Applications`。Qt 动态库和插件已经放进应用包；Agent 入口位于 `vibeled.app/Contents/Resources/hooks/`。DMG 必须在 macOS 上构建；Windows 脚本生成 ZIP。

## 构建依赖及参数

Windows 构建需要 Python 3.10+、Visual Studio C++ 工具链、CMake、Ninja，以及带 Core、Widgets、Network、SerialPort、Test 模块的 Qt：

```powershell
.\package-vibeled.cmd --qt D:/dev.down/Qt5.15.14
./dist/vibeled/vibeled.exe
./dist/vibeled/vibeled.exe --help
```

Windows 默认自动使用本机 `D:/dev.down/Qt5.15.14`，也可传入 `--qt` 或设置 `VIBELED_QT`。脚本使用 `windeployqt` 收集 Qt，并从 Visual Studio 的可分发目录复制 MSVC DLL；会实际解压 ZIP，在移除 Qt 开发目录的 PATH 下验证 CLI 启动。另保留 `dist/vibeled/` 供本机运行。更新占用中的 EXE/DLL 时先改为带时间戳的废弃文件名，不强制结束用户进程；这些废弃文件不会进入 ZIP。

应用图标采用深蓝底的 RGB 发光 V，Windows EXE、Qt 窗口和 macOS 应用使用同一套图形。`tool/assets/` 保存透明原图、16–1024 像素 PNG、多尺寸 ICO 和含 Retina 尺寸的 ICNS。原图由内置 imagegen 生成，完整提示词见 `tool/assets/icon-design.json`；只需修改原图后运行以下命令，即可用 Qt 重新生成图标尺寸并打包，无需额外的图片处理依赖：

```powershell
python tools/build_vibeled.py --refresh-icons --qt D:/dev.down/Qt5.15.14
```

正在运行的旧实例会保留原图标；正常退出并重新启动后显示新图标。

macOS 构建需要 Python 3.10+、Xcode Command Line Tools、CMake、Qt 5.15 或 Qt 6（含上述模块）。默认查找 Homebrew、`~/Qt/` 和 PATH 中的 Qt，也可显式指定：

```sh
bash package-vibeled.command --qt /path/to/Qt/macos
open dist/vibeled.app
dist/vibeled.app/Contents/MacOS/vibeled status
```

macOS 脚本调用 `macdeployqt`，临时签名并验证应用，再用系统 `hdiutil` 生成和校验压缩 DMG，卷内提供 Applications 拖放入口。Intel/Apple Silicon 分别使用相应架构的 Qt 和工具链构建。无需蓝牙权限，也无需通过 Terminal.app 或 LaunchServices 转发 CLI。跨机器公开发布时可另行进行开发者签名和公证；本地打包不包含公证。

两个一键入口都支持 `--no-pause`，用于终端或自动构建时结束后不等待按键。也可直接运行 `python tools/build_vibeled.py`（macOS 用 `python3`）；`--no-package` 仅编译与测试。

## 原命令行保持兼容

只将原可执行文件名替换为 `vibeled`：

下方 `vibeled` 可替换为构建结果的完整路径，也可将可执行文件所在目录加入 PATH。

```sh
vibeled scan
vibeled scan --all
vibeled --device XGAI_LED --timeout 15 status
vibeled set '#33AAFF'
vibeled set orange --altcolor blue --on-ms 200 --off-ms 800 --duration-ms 10000 --level 4
vibeled save deploy-ok '#00CC44' --altcolor '#0033FF' --on-ms 150 --off-ms 150 --duration-ms 5000 --level 2
vibeled play deploy-ok --level 5
vibeled delete deploy-ok
vibeled list
vibeled status
vibeled raw heartbeat --level 5
vibeled off
```

- `--device` 和 `--timeout` 仍放在子命令前。超时默认 15 秒，涵盖排队、串口打开和设备响应。
- `--device` 现在接受 `COM5`、`/dev/cu.usbmodem…`、USB 序列号或设备名称。`XGAI_LED` 和 `vibeled` 都表示自动选择唯一接收器。旧蓝牙 UUID/地址不再对应 USB 端口，请用 `scan` 获取新标识。有多个匹配设备时报错，避免选错。
- `scan` 列出接收器的 CDC 端口，`scan --all` 列出所有串口；扫描不打开设备。
- `--help`、`-h`、`help`、`--version` 和参数校验在本机完成。
- `raw COMMAND...` 接受旧文本协议，包括可选的 `XGAI_LED:` 前缀。
- `wifi SSID PASSWORD` 保留参数解析，但返回明确的“不支持/使用 USB 无需配网”错误；`getip` 或 `raw getip` 返回 `no ip`。
- 成功退出码为 `0`，传输/设备发现失败为 `1`，参数或灯光命令错误为 `2`。CLI 等待真实设备响应；GUI 已运行时由 GUI 排队执行并返回结果，命令也出现在操作记录中。

Qt 界面包含颜色选择、交替色、亮灭时间、总时长、level、快捷灯效的保存/播放/删除、状态查询和原始命令。Windows 全局关灯默认 `Ctrl+Alt+L`，macOS 保留 `Control+Command+L`，可在界面修改或恢复默认。快捷键冲突时保留原注册。全局快捷键仅在 GUI 运行时有效。

## 灯效规则

从固件 `0.13.1` 起，同一份固件同时向两种板子的板载 RGB 灯输出相同颜色，无需切换板型：

| 板子 | RGB 数据引脚 | 线上颜色顺序 |
| --- | --- | --- |
| Waveshare ESP32-S3-Zero | GPIO21 | RGB |
| ESP32-S3 SuperMini（GPIO48 版） | GPIO48 | GRB |

两种板子都保留 GPIO21、GPIO48 用于灯光，不应再接其他外设。Flash/PSRAM 构建配置仍按实际容量选择；4 MB Flash、2 MB Quad PSRAM 使用 `q2-f4`。GPIO 7/8/9 继续驱动旧硬件的外接绿/黄/红灯，它们不是 SuperMini 的板载红、蓝灯。颜色名称支持 `black white red green blue yellow cyan magenta orange purple` 或 6 位十六进制。

启动时清空两个 RGB 输出；仅在颜色改变时发送一次数据，发送后保持低电平，不进行周期刷新。按 [SuperMini 原理图](https://fomenko.kyiv.ua/wp-content/uploads/2026/03/007217_ESP32-S3_Supermini_schematic-1555x1080.png)，独立红灯与 RGB 数据共用 GPIO48、高电平点亮，因此固件运行后不再常亮，但 RGB 更新期间的极短脉冲无法独立消除。上电至固件初始化前的灯态不由此驱动控制。

该版 SuperMini 的蓝灯由 TP4054 的充电状态引脚直接驱动，没有连接 ESP32 GPIO，固件无法关闭它或阻止无电池时可能出现的闪烁。要彻底熄灭只能遮光或修改硬件（例如拆除蓝灯或其串联电阻），软件不能替代。其他同名板子需要核对接线。Zero 的 GPIO 和颜色顺序参见[微雪示例](https://docs.waveshare.com/ESP32-ESP-IDF-Tutorials/Rmt-Drive-Ws2812)及[官方 FAQ](https://docs.waveshare.net/ESP32-S3-Zero/FAQ/)，SuperMini 的 GRB 顺序参见 [ESPHome 板卡配置](https://devices.esphome.io/devices/tenstar-robot-esp32-s3-supermini/)。

`on-ms` 默认 1000，必须大于 0；`off-ms` 默认 0（常亮）；`duration-ms` 默认 0（无限）；`altcolor` 默认黑色；level 默认 3，范围 1–5。时间参数接受无符号 32 位整数。

高 level 覆盖低 level。被覆盖的灯效继续计时，高 level 到期后显示当时仍有效的最高低 level。高 level 的灭相显示自己的交替色，不透出低 level。`set black --level N` 清除单层（未设置非黑色交替色时），`off` 清除全部层。

保留内置 `green`（低亮绿色、5 分钟）、`yellow`、`red`、`blue`、`slow-blue`（500/500 ms）和 `heartbeat`（120/880 ms）。`set green` 使用完整绿色，`play green` 使用原来的低亮预设。

最多保存 8 个用户灯效，名称为 1–15 个 ASCII 字母、数字、`_` 或 `-`，按名称忽略大小写查找。用户预设可覆盖同名内置灯效，删除用户预设后内置灯效重新可见。保存后立即播放；level 不写入预设。灯效存入当前固件数据分区的 `xgai.effects`，正常更新随数据分区复制，重新启动后保留；活动灯效在重启后清空。旧项目分区布局不同，本次合并不迁移旧分区中的 Wi-Fi/灯效数据。

灯效定时使用单独任务和单调时钟，常亮/关闭时无需周期刷新；RMT 发完一个完整帧就释放其时钟锁。正在录音、更新固件或迁移数据库时，`save`/`delete` 返回 busy，可在空闲时重试；普通灯控继续可用。

## Agent Python 接口

以下三个入口等价，沿用 `--agent NAME EVENT` 和 stdin JSON：

```sh
python tools/agent_hook.py --agent codex PreToolUse
python tools/logled_agent_hook.py --agent codex PermissionRequest
python tools/vibeled_agent_hook.py --agent codex Stop
```

不需要安装 Python 第三方包。JSON 的 `hook_event_name` 优先于位置参数；未知事件忽略；所有入口始终静默、返回 0，设备离线也不会阻塞或中断 Agent。脚本后台启动 `vibeled`，由 Qt 串行执行 CDC 请求。保留 Codex、Claude、Kiro 事件映射：

| 事件状态 | 灯效 |
| --- | --- |
| SessionStart / agentSpawn | 绿色短闪 1.8 秒 |
| UserPromptSubmit / PreToolUse / PostToolUse / SubagentStart 等 | 低亮黄色 |
| Kiro userPromptSubmit / postToolUse | 更低亮黄色 |
| PermissionRequest / PermissionDenied / Elicitation | 黄色 350/350 ms 闪烁 |
| Stop / stop | 绿色 500/500 ms 闪烁，60 秒后结束 |
| PostToolUseFailure / StopFailure | 红色快闪 10 秒 |
| SessionEnd | 全部关灯 |

环境变量：

| 变量 | 用途 |
| --- | --- |
| `VIBELED_TOOL` | 指定 EXE、macOS 可执行文件或 `.app` 路径 |
| `VIBELED_APP` / `LOGLED_APP` | 兼容的路径设置；值需指向新版程序 |
| `VIBELED_DEVICE` | 多设备时指定端口或序列号 |
| `VIBELED_HOOK_EVENT` | 没有 JSON/位置事件时的后备事件 |
| `VIBELED_HOOK_STATE_DIR` | 去重状态目录 |
| `VIBELED_HOOK_DEDUP_SECONDS` | 相同灯效请求的去重窗口，默认 15 秒；0 禁用 |

后三项以及 `DEVICE` 也接受 `LOGLED_` 前缀；`VIBELED_` 优先。旧 `LOGLED_HOOK_OPEN` 不再使用，因为不经过 macOS `open`。自动查找顺序包含 PATH、仓库的 `dist/`、构建目录、macOS 的 `~/Applications` 和 `/Applications`。正常只需替换原 hook 的脚本路径，保留事件条目。

## CDC 与原 Buddy App

复用现有 USB 身份 `05AC:0220`（也能发现旧 `CAFE:4016`）和 RBP/3 管理通道。新增 `INFO.led_api = 1`；操作码 `0x470` 的请求是 `{"command":"set red 1000 0 0 --level 3"}`，响应是 `{"text":"ok ..."}`。需要正常 HELLO / CLOSE 管理会话，所有帧验证 CRC32C、长度、会话和请求号。键盘/麦克风描述符及原遥控器命令不变。

一个 CDC 串口同时只有一个进程可以占用。`vibeled` 的 GUI、CLI 和 Agent 请求通过进程锁/本地 IPC 排队，并在每次请求后关闭管理会话和串口。Buddy App 或串口监视器长时间占用端口时，请先退出它；当前没有修改另一个仓库的 Buddy App 来代理灯光命令。界面空闲不占串口；短暂的管理会话期间后台自动配对遵循原固件的管理策略。

Windows 在接收器重启后可能需要数十秒重新枚举复合 USB 设备。升级的自动健康确认仍要求存储、蓝牙和 USB 均就绪，但等待期限由 20 秒延长为 120 秒，避免健康固件在 USB 就绪前被误判回滚。

测试入口：

```sh
python tools/build_vibeled.py --qt /path/to/Qt
python tests/test_vibeled_hook.py
python tools/generate_models.py --check
python tests/test_build_config.py
cmake -S tests -B build/adapter-tests
cmake --build build/adapter-tests
ctest --test-dir build/adapter-tests --output-on-failure
```

连接真实接收器后的测试会改变灯光并保存 `vibe-test` 预设；重启或固件更新后再检查该预设并删除：

```sh
python tests/hardware_vibeled.py dist/vibeled/vibeled.exe --device COM5
# 重新启动/更新固件，等待 USB 重新出现后：
python tests/hardware_vibeled.py dist/vibeled/vibeled.exe --device COM5 --check-persisted
```

本次在 Windows、Qt 5.15.14 和 q2-f4 接收器上验证：17 项 Qt/灯效测试、5 项 Agent 测试、原固件 5 组 C 回归测试及 2 项构建配置测试通过。实机覆盖命令行、GUI 转发、24 请求并发、Agent 事件链、优先级恢复、双色定时、错误 CRC/长度帧拒绝、CDC 升级及跨重启预设保存；用户确认实际颜色与闪烁正常。macOS 已提供源码和打包路径，尚未在 macOS 编译或实机验证；遥控器按键和语音本次仅运行原有主机回归测试。
