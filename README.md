# kovaak_controller — KovaaK's 原生手柄桥（APEX SIMULATOR 同源输入）

让你用**手柄原生输入**玩 KovaaK's FPS Aim Trainer，摇杆→视角的转速、延迟与
APEX SIMULATOR（controlsphere）**1:1 一致**。

## 原理（与 Steam Input/JoyToKey 类"摇杆转鼠标"方案的本质区别）

```
手柄 HID (16-bit 轴, 0 死区, 设备报告率 8kHz)
   │  RawInput RIDEV_INPUTSINK（后台接收，XInput 绕过，双精度直读）
   ▼
Apex 灵敏度数学（APEX SIMULATOR player_controller.apex_controller_aim_64 逐行移植）
   │  Linear 曲线 | Hip 4 = 255°/s | ADS 3 = 191.25°/s
   │  死区 0%/0% | 俯仰 = 偏航×0.8 | 硬上限 510°/s (85 RPM)
   ▼
°/s ÷ KovaaK's deg/count（自动读 PrimaryUserSettings.json 推导）
   │  例：CS 刻度 XSens 1.8 → 0.0396°/count → 满杆 = 6439 counts/s
   ▼
SendInput 相对鼠标计数（16000Hz QPC 循环）→ KovaaK's (UE4 Raw Input)
```

- **输入侧 100% 原生**：直接解析手柄 HID 报告，16-bit 摇杆分辨率（1 计数 = 1/32768），
  无 XInput 死区/量化，无 Steam Input 转换层，无摇杆→光标语义。
- **灵敏度 100% 同源**：与模拟器同一套 Apex 数学（`--selftest` 33 项断言全部对齐）。
- **延迟同级**：默认 16000Hz 注入循环（对应模拟器 `Engine.max_fps=16000` 的逐帧采样），
  按键在 HID 报告到达的瞬间（读取线程内）即注入。
- 输出通道说明：KovaaK's 没有插件 API 也不读手柄，相对鼠标计数是它唯一的外部输入通道
  ——这也是真实鼠标的同一通道，逐计数等价，不是"光标移动转换"。

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

## 按键映射（Button Puncher，与你的 Apex 配置一致）

| 手柄 | 注入 | 说明 |
|---|---|---|
| RB / R1 | 鼠标左键 | 开火 |
| LB / L1 | 鼠标右键 | 开镜（同时 ADS 数学 191.25°/s 生效）|
| A / 叉 | Space | 跳跃 |
| X / 方块 | R | 换弹 |
| Menu / Options | Esc | KovaaK's 菜单 |
| LS / L3 按下 | Shift | 冲刺 |
| 左摇杆 | WASD | 移动（阈值 0.4，可调 `move_threshold`）|
| 右摇杆 | 视角 | Apex 数学直出 |

按键默认按设备 VID 自动选择 Xbox/PlayStation 布局；不匹配时用 `--diag`
看实机 usage ID，然后在 `config.json` 的 `buttons` 里覆盖：

```json
"buttons": { "fire": 6, "ads": 5, "jump": 1, "reload": 3, "escape": 8, "sprint": 9 }
```

## 配置（config.json，与 exe 同目录）

| 键 | 默认 | 说明 |
|---|---|---|
| `loop_hz` | 16000 | 注入循环频率（与模拟器同级；8000 可降 CPU）|
| `settings_path` | 自动 | KovaaK's PrimaryUserSettings.json 路径（默认自动探测 C:/D: Steam 库）|
| `deg_per_count_override` | 0 | >0 时固定换算，不读游戏设置 |
| `always_inject` | false | 关闭前台闸门（默认仅 KovaaK's 前台时注入）|
| `invert_y` | false | 俯仰反转（也会读取游戏内 YInvert）|
| `move_threshold` | 0.4 | 左摇杆→WASD 触发阈值（带迟滞）|
| `apex.*` | 用户 Apex 配置 | look_curve / hip_aim_speed / ads_aim_speed / ads_scalar / deadzone / pitch_ratio |
| `target_processes` | KovaaK's | 前台闸门进程名列表 |

**设置热重载**：在 KovaaK's 里改灵敏度后，桥自动重读并重算换算
（摇杆→屏幕转速始终 1:1，不受游戏灵敏度影响）。

## 构建

```powershell
powershell -File build.ps1   # w64devkit MinGW g++ (GCC 14)，与 APEX SIMULATOR 同工具链
```

源码：`src/rawinput_reader.*`（HID 读取，gdext_rawinput 移植+按钮/断连）、
`src/apex_sens.h`（Apex 数学移植）、`src/kovaak_config.*`（设置推导+公式求值）、
`src/injector.h`（SendInput）、`src/main.cpp`（16000Hz 主循环/闸门/映射/自检）。

## 注意事项

1. **禁用 Steam Input**（重要）：Steam → FPSAimTrainer → 属性 → 控制器 →
   "禁用 Steam Input"，否则 Steam 会抢先把手柄转换成鼠标/键盘造成双重输入。
2. **权限一致**：若 KovaaK's 以管理员运行，本工具也要管理员运行（UIPI 会拦截低权限注入）。
3. **ADS 缩放**：KovaaK's 武器开镜有自己的 FOV 缩放，屏幕上的转动观感会随 FOV 变化；
   摇杆→°/s 的物理转速与 Apex/模拟器 1:1（这是灵敏度的一致定义）。
   想要完全相同的观感，把 KovaaK's FOV 调成与 Apex 一致（110 水平）。
4. 0 死区意味着摇杆漂移会直通（与模拟器行为一致）；如有漂移可在 `config.json`
   设 `apex.deadzone_in`（如 0.02）。
5. 已实测：本机手柄（VID_413D，8kHz 轮询）HID 直读 ~7990Hz、16-bit 位深。
