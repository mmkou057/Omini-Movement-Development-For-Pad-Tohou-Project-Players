# Touhou Omnidirectional Movement（东方Project 手柄全向移动）

给只能 **8 向移动**的东方正作加上了摇杆全向移动功能。
**任意角度匀速移动**：把连续摇杆方向按帧调制成游戏原生的 8 向按键序列，帧均速度即指向摇杆方向。

- `dinput8.dll` 代理由纯C写成，放进游戏目录即生效，**不需要外部注入器常驻**
- 物理键盘始终可用；摇杆回中后键盘方向立即恢复
- 兼容编号非标准的第三方手柄
- 已在 **TH15 东方绀珠传** 实测可走出 30° 、15°等任意方向

## 工作原理

1. **DLL 代理劫持**：Windows 优先加载游戏目录下的 `dinput8.dll`。本代理把 `DirectInput8Create` 转发给系统真 DLL，同时 hook 返回的 DirectInput 设备对象的 `GetDeviceState`（vtable[9]）。
2. **搭游戏手柄设备的便车**：不自行创建设备，而是等游戏枚举并创建好手柄设备后接管它的读取。
3. **中和 + 注入**：
   - 手柄的真实读取结果被复制为内部唯一数据源，交还给游戏的缓冲被中和为静止态，游戏自身的 4/8 向手柄路径永远看到静止，不会与注入冲突；
   - 键盘读取时，按手柄方向把 8 向键注入键盘缓冲。
4. **PWM 凸包分解**（见 [pwm.c](./pwm.c)）：4 个主向 + 4 个对角向量构成正八边形，任意摇杆向量可写成相邻两个顶点向量的凸组合 `v = (1-a)·vA + a·vB`。每帧只按 Bresenham 占空比输出 A 或 B，帧均速度收敛到 `v`。另外，东方至今的所有作品均为匀速移动，故摇杆幅值不参与调制（超过死区即满速）。

## 目录结构

| 路径 | 说明 |
|---|---|
| [padhook.c](./padhook.c) | DLL 代理主体：DirectInput hook、设备分流、轴归中、按钮映射 |
| [pwm.c](./pwm.c) / [pwm.h](./pwm.h) | 八向凸包分解 + Bresenham 占空比 |
| [padhook.def](./padhook.def) | 导出 `DirectInput8Create` |
| [PadSwitch.cs](./PadSwitch.cs) | 部署开关 GUI（扫描游戏 / 启用 / 取消 / 启动） |
| [test_sim.c](./test_sim.c) | 全链路控制台模拟测试（0°/30°/45°/90°、死区、POV、按钮） |
| [test_load.c](./test_load.c) | DLL 加载与导出符号烟雾测试 |
| [build_dll.bat](./build_dll.bat) | 编译 `dinput8.dll`（i686 MinGW） |
| [build_switch.bat](./build_switch.bat) | 编译 `PadSwitch.exe`（.NET Framework csc） |
| [padhook.ini.example](./padhook.ini.example) | 配置样例 |
| [release/](./release) | 实测通过的预编译成品 |
| [diag/](./diag) | 诊断用全透明代理 `padprobe`（排查环境问题时留档） |
| [legacy/](./legacy) | 旧版外部 SendInput 注入器（已被 DLL 方案取代，存档） |

## 快速使用（推荐）

1. 从 [release/](./release) 取出 `dinput8.dll` 和 `PadSwitch.exe`，放在**同一目录**。
2. 双击 `PadSwitch.exe`：
   - 下拉框选择游戏（自动扫描程序目录上一级的 `..\test` 与 `..\Tohou\STG`，也可点「浏览」手动指定任意 `th*.exe`（或`thprac.v*.exe`））；
   - 勾选「启用全向移动」→ 自动复制 `dinput8.dll` 并写入 `padhook.ini`（`mode=3`）；
   - 取消勾选 → 删除 `dinput8.dll` / `padhook.ini` / `padhook.log`，恢复原版,实测有概率闪退，重启游戏即可；
   - 切换启用状态前若游戏正在运行，程序会提示并可自动结束游戏进程。
3. 启动游戏。

> 注意：**替换 `dinput8.dll` 前必须先退出游戏**，运行中的进程会锁定文件，可能复制出 0 字节坏 DLL。

## 手动部署

把 `dinput8.dll` 复制到 `th15.exe`（或其他正作主程序）同目录，并在同目录放置 `padhook.ini`：

```ini
[padhook]
mode=3
btn_shoot=0
btn_bomb=1
btn_slow=4
btn_pause=7
```

删除该 DLL 即完全卸载，游戏文件本身不被修改。

## 配置说明（padhook.ini）

节名固定为 `[padhook]`，文件需为 **无 BOM** 编码（PadSwitch 写出的即为无 BOM UTF-8）。

| 键 | 默认值 | 说明 |
|---|---|---|
| `mode` | `0` | `0` 完全透明（等同未安装）；`1` 仅压制手柄（诊断）；`2` 仅 hook 键盘并原样透传（诊断）；`3` 完整全向移动 |
| `btn_shoot` | `0` | 射击按钮编号（0 基） |
| `btn_bomb` | `1` | 弹幕 / BOMB 按钮编号 |
| `btn_slow` | `4` | 低速移动按钮编号 |
| `btn_pause` | `7` | 暂停按钮编号 |
| `axis_min` / `axis_max` | 自动 | 可选，手动指定轴范围；缺省时以自然归中点 EMA 自适应（兼容 0..65535、±1000 等各种手柄） |

**按钮校准**：第三方手柄按钮编号往往非标准。可用诊断版 DLL（`-DPADHOOK_DIAG` 编译）运行游戏，`padhook.log` 中会以 hex 转储 32 字节按钮位，按下手柄各键即可读出真实编号，再填入 ini。

## 从源码编译

### dinput8.dll（32 位）

需要 i686 的 MinGW-w64 GCC（如 WinLibs i686 版本）。把工具链解压为仓库上一级的 `..\mingw32`（使 `..\mingw32\bin\gcc.exe` 存在），或把 gcc 加入 PATH（也可用 `set GCC=路径` 覆盖），然后：

```bat
build_dll.bat
```

脚本会编译 [padhook.c](./padhook.c) + [pwm.c](./pwm.c) 并用 objdump 验证 `DirectInput8Create` 导出。诊断版可手动加 `-DPADHOOK_DIAG`（高频日志，仅排查时使用）。

### PadSwitch.exe

需要 Windows 自带的 .NET Framework 4（csc.exe），无需安装 SDK：

```bat
build_switch.bat
```

> 提示：PadSwitch 必须以 **x86** 编译，才能正确枚举 32 位游戏进程的模块。

## 测试

- `test_sim.c` 直接 `#include "padhook.c"` 编成控制台程序，每帧从模板重建手柄缓冲，验证 0°/45°/90°/30°、死区、POV、按钮映射的整条 PWM 链路：

```bat
..\mingw32\bin\gcc.exe -m32 -o test_sim.exe test_sim.c pwm.c -lm -lkernel32 -luser32 -lgdi32 -lwinmm
.\test_sim.exe
```

- `test_load.c` 验证 DLL 可加载且导出符号可解析：

```bat
..\mingw32\bin\gcc.exe -m32 -o test_load.exe test_load.c
.\test_load.exe
```

## 兼容性

- 目标：使用 **DirectInput8 读键盘 + winmm 读手柄** 的东方正作。
- TH15 东方绀珠传和TH08 东方永夜抄已实测；其余正作理论上同一 DLL 通用，按钮编号按 ini 校准即可。
- 第三方非标准手柄若 XInput 读取全零也不受影响——本方案直接复用游戏自己的 DirectInput 设备数据。

## 故障排查

- **菜单自动滚动 / 摇杆异常**：轴未正确回中。删掉 `padhook.ini` 中手写的 `axis_min/axis_max`，让 EMA 自然归中重新学习。
- **启用后键盘失灵或游戏崩溃**：确认使用的是本仓库 [release/](./release) 的成品（58311 字节）；早期版本 hook 了 `SetProperty`（vtable[6]）会导致 0xc00000005，当前版本只 hook `GetDeviceState`。
- **游戏静默退出**：自行用其他工具链编译时注意，32 位 MinGW 的 `__thread` 会引入对 `libgcc_s_dw2-1.dll` 的动态依赖；本项目已改用 kernel32 `TlsAlloc` 规避。
- **DLL 不生效**：检查是否放在游戏主程序**同目录**、游戏是否为 32 位（本 DLL 为 32 位）、替换前游戏进程是否已退出。

## 免责声明

本项目仅用于学习 DirectInput hook 与输入调制技术，不包含、不分发任何游戏资源。东方Project 著作权归上海アリス幻樂団所有，请通过合法渠道购买游戏。
