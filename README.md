# kovaak_controller — KovaaK's 原生手柄桥

**[English](README_EN.md)** | 中文

让你用**手柄原生输入**玩 KovaaK's FPS Aim Trainer：摇杆直接驱动视角，
转速、死区、响应曲线与延迟全部可配置、可推导，默认参数复刻
Apex Legends 主机版手柄的视角手感。

## 原理（与 Steam Input/JoyToKey 类"摇杆转鼠标"方案的本质区别）

```
手柄 HID (16-bit 轴, 0 死区, 设备报告率最高 8kHz)
   │  RawInput RIDEV_INPUTSINK（后台接收，绕过 XInput，双精度直读）
   ▼
视角控制模型（本项目完整实现，参数全部可配置）
   │  响应曲线 | 腰射转速档 4 = 255°/s | 开镜转速档 3 = 191.25°/s
   │  死区 0%/0% | 俯仰 = 偏航×0.8 | 硬上限 510°/s (85 RPM)
   ▼
°/s ÷ KovaaK's deg/count（自动读 PrimaryUserSettings.json 推导）
   │  例：CS 刻度 XSens 1.8 → 0.0396°/count → 满杆 = 6439 counts/s
   ▼
SendInput 相对鼠标计数（16000Hz QPC 循环）→ KovaaK's (UE4 Raw Input)
```

- **输入侧 100% 原生**：直接解析手柄 HID 报告，16-bit 摇杆分辨率
  （1 计数 = 1/32768 摆幅），无 XInput 死区/量化，无 Steam Input 转换层，
  无摇杆→光标语义。
- **视角模型完整自实现**：5 档响应曲线、腰射/开镜独立转速、径向死区、
  俯仰比、转速硬上限，全部参数开放配置（见下节），并由 `--selftest`
  的 33 项断言逐一验证。
- **延迟同级**：默认 16000Hz 注入循环（摇杆状态以 62.5µs 粒度重采样），
  按键在 HID 报告到达的瞬间（读取线程内）即注入。
- 输出通道说明：KovaaK's 没有插件 API 也不读手柄，相对鼠标计数是它唯一的
  外部输入通道——这也是真实鼠标的同一通道，逐计数等价，不是"光标移动转换"。

## 视角控制模型

摇杆偏转 → 视角角速度的完整数学（`src/apex_sens.h`）：

1. **幅度**：取摇杆向量圆形幅度 `√(rx² + ry²)`，钳到 1；
2. **死区**：径向重映射 `(m − dz_in) / (1 − dz_in − dz_out)`（默认 0/0 直通）；
3. **响应曲线**（全部过 (0,0) 与 (1,1)，满杆输出满速）：

   | 曲线 | 公式 | 手感 |
   |---|---|---|
   | Classic (0) | `3x² − 2x³`（Hermite 平滑） | 中段快、两端缓 |
   | Steady (1) | `x^2.5` | 小输入更稳 |
   | Linear (2) | `x`（恒等，默认） | 输入输出严格成正比 |
   | Wide (3) | `x^1.4` | 中段更饱满 |
   | Precise (4) | `x^1.8` | 小输入更细腻 |

4. **转速**：`角速度 = 曲线输出 × 转速档 × 63.75°/s`，开镜再乘 `ads_scalar`，
   全局硬上限 510°/s（85 RPM）。默认腰射档 4 = 255°/s、开镜档 3 = 191.25°/s；
5. **方向**：按原始摇杆向量方向分配偏航/俯仰，俯仰恒为偏航的 80%
   （`pitch_ratio` 可调）。

该模型即主机 FPS 通用的"视角速度"灵敏度定义：摇杆摆幅决定的是**°/s**，
与游戏内鼠标灵敏度无关——换灵敏度后换算自动重算，手感不变。

## 使用

```powershell
# 1. 先自检（无需手柄，校验数学与 KovaaK's 设置推导）
.\build\kovaak_controller.exe --selftest

# 2. （建议首测）dry-run：只计算不注入，摇杆偏转时看注入计数变化
.\build\kovaak_controller.exe --dry-run

# 3. 正式使用：启动后打开 KovaaK's（前台=游戏时才注入，切出自动暂停）
.\build\kovaak_controller.exe

# 手柄按键映射不对时：诊断实机 HID usage ID
.\build\kovaak_controller.exe --diag
```

Ctrl+C 退出（自动释放全部注入按键）。CPU 不足时用 `--loop-hz 8000` 降自旋频率。

## 按键映射（Button Puncher 布局）

| 手柄 | 注入 | 说明 |
|---|---|---|
| RB / R1 | 鼠标左键 | 开火 |
| LB / L1 | 鼠标右键 | 开镜（同时开镜转速档生效）|
| A / 叉 | Space | 跳跃 |
| X / 方块 | R | 换弹 |
| Menu / Options | Esc | KovaaK's 菜单 |
| LS / L3 按下 | Shift | 冲刺 |
| 左摇杆 | WASD | 移动（阈值 0.4，可调 `move_threshold`）|
| 右摇杆 | 视角 | 视角模型直出 |

按键默认按设备 VID 自动选择 Xbox/PlayStation 布局；不匹配时用 `--diag`
看实机 usage ID，然后在 `config.json` 的 `buttons` 里覆盖：

```json
"buttons": { "fire": 6, "ads": 5, "jump": 1, "reload": 3, "escape": 8, "sprint": 9 }
```

## 配置（config.json，与 exe 同目录）

| 键 | 默认 | 说明 |
|---|---|---|
| `loop_hz` | 16000 | 注入循环频率（8000 可降 CPU）|
| `settings_path` | 自动 | KovaaK's PrimaryUserSettings.json 路径（默认自动探测 C:/D: Steam 库）|
| `deg_per_count_override` | 0 | >0 时固定换算，不读游戏设置 |
| `always_inject` | false | 关闭前台闸门（默认仅 KovaaK's 前台时注入）|
| `invert_y` | false | 俯仰反转（也会读取游戏内 YInvert）|
| `move_threshold` | 0.4 | 左摇杆→WASD 触发阈值（带迟滞）|
| `apex.*` | 见上表 | 视角模型参数：look_curve / hip_aim_speed / ads_aim_speed / ads_scalar / deadzone / pitch_ratio |
| `target_processes` | KovaaK's | 前台闸门进程名列表 |

**设置热重载**：在 KovaaK's 里改灵敏度后，桥自动重读并重算换算
（摇杆→屏幕转速始终恒定，不受游戏灵敏度影响）。

## 构建

```powershell
powershell -File build.ps1   # w64devkit MinGW g++ (GCC 14)
```

源码：`src/rawinput_reader.*`（HID 报告解析与设备管理）、
`src/apex_sens.h`（视角控制模型）、`src/kovaak_config.*`（设置推导+公式求值）、
`src/injector.h`（SendInput）、`src/main.cpp`（16000Hz 主循环/闸门/映射/自检）。

## 注意事项

1. **禁用 Steam Input**（重要）：Steam → FPSAimTrainer → 属性 → 控制器 →
   "禁用 Steam Input"，否则 Steam 会抢先把手柄转换成鼠标/键盘造成双重输入。
2. **权限一致**：若 KovaaK's 以管理员运行，本工具也要管理员运行（UIPI 会拦截低权限注入）。
3. **ADS 缩放**：KovaaK's 武器开镜有自己的 FOV 缩放，屏幕上的转动观感会随 FOV 变化；
   摇杆→°/s 的物理转速是恒定的（这是灵敏度的一致定义）。
   想要与目标游戏相同的观感，把 KovaaK's FOV 调成一致（如 Apex 的 110 水平）。
4. 0 死区意味着摇杆漂移会直通；如有漂移可在 `config.json`
   设 `apex.deadzone_in`（如 0.02）。
5. 已实测：本机手柄（VID_413D，8kHz 轮询）HID 直读 ~7990Hz、16-bit 位深。
