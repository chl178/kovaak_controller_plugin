# kovaak_controller — Native controller bridge for KovaaK's

**English** | [中文](README.zh-CN.md)

Play KovaaK's FPS Aim Trainer with **native controller input**: the right stick
drives the camera directly, with fully configurable and derivable turn speed,
deadzones, response curves and latency. Defaults replicate the console
controller camera feel of Apex Legends.

## How it works (and why it differs from Steam Input / JoyToKey-style "stick-to-mouse")

```
Controller HID (16-bit axes, 0 deadzone, up to 8kHz device polling)
   │  RawInput RIDEV_INPUTSINK (background capture, bypasses XInput,
   ▼  double-precision direct reads)
Camera control model (fully implemented in this project, all parameters configurable)
   │  Response curve | Hip aim level 4 = 255°/s | ADS aim level 3 = 191.25°/s
   │  Deadzone 0%/0% | Pitch = yaw × 0.8 | Hard cap 510°/s (85 RPM)
   ▼
°/s ÷ KovaaK's deg/count (auto-derived from PrimaryUserSettings.json)
   │  e.g. CS scale XSens 1.8 → 0.0396°/count → full deflection = 6439 counts/s
   ▼
SendInput relative mouse counts (16000Hz QPC loop) → KovaaK's (UE4 Raw Input)
```

- **100% native input**: gamepad HID reports are parsed directly — 16-bit stick
  resolution (1 count = 1/32768 of deflection), no XInput deadzone/quantization,
  no Steam Input conversion layer, no stick→cursor semantics.
- **Self-contained camera model**: 5 response curves, independent hip/ADS turn
  speeds, radial deadzones, pitch ratio and a hard speed cap — all parameters
  are configurable (see next section) and verified by the 33 assertions in
  `--selftest`.
- **Comparable latency**: default 16000Hz injection loop (stick state is
  resampled at 62.5µs granularity); buttons are injected the moment their HID
  report arrives (inside the reader thread).
- Output channel note: KovaaK's has no plugin API and does not read gamepads;
  relative mouse counts are its only external input channel — the very same
  channel a real mouse uses, count-for-count equivalent, not a "cursor
  movement" conversion.

## Camera control model

The complete stick-deflection → angular-velocity math (`src/apex_sens.h`):

1. **Magnitude**: circular stick magnitude `√(rx² + ry²)`, clamped to 1;
2. **Deadzone**: radial remap `(m − dz_in) / (1 − dz_in − dz_out)`
   (defaults 0/0 → pass-through);
3. **Response curve** (all pass through (0,0) and (1,1); full deflection =
   full speed):

   | Curve | Formula | Feel |
   |---|---|---|
   | Classic (0) | `3x² − 2x³` (Hermite smoothstep) | fast middle, soft ends |
   | Steady (1) | `x^2.5` | steadier at small inputs |
   | Linear (2) | `x` (identity, default) | strictly proportional |
   | Wide (3) | `x^1.4` | fuller middle range |
   | Precise (4) | `x^1.8` | finer at small inputs |

4. **Speed**: `angular velocity = curve output × aim level × 63.75°/s`; ADS is
   additionally scaled by `ads_scalar`, with a global hard cap of 510°/s
   (85 RPM). Defaults: hip level 4 = 255°/s, ADS level 3 = 191.25°/s;
5. **Direction**: yaw/pitch are distributed along the raw stick direction;
   pitch is always 80% of yaw (adjustable via `pitch_ratio`).

This is the standard console-FPS "camera speed" sensitivity definition: stick
deflection determines **°/s**, independent of the in-game mouse sensitivity —
when you change sensitivity, the conversion is recomputed automatically and the
feel stays the same.

## Usage

```powershell
# 1. Self-test first (no controller needed; validates the math and the
#    KovaaK's settings derivation)
.\build\kovaak_controller.exe --selftest

# 2. (Recommended first run) dry-run: compute but don't inject; watch the
#    injection counts change as you deflect the stick
.\build\kovaak_controller.exe --dry-run

# 3. Normal use: start it, then open KovaaK's (injects only while the game
#    is in the foreground; auto-pauses when you switch away)
.\build\kovaak_controller.exe

# If button mapping is wrong: diagnose the device's real HID usage IDs
.\build\kovaak_controller.exe --diag
```

Ctrl+C to quit (all injected keys are released automatically). Use
`--loop-hz 8000` to reduce CPU spin if needed.

## Button mapping (Button Puncher layout)

| Controller | Injects | Notes |
|---|---|---|
| RB / R1 | Left mouse | Fire |
| LB / L1 | Right mouse | ADS (also switches to the ADS speed level) |
| A / Cross | Space | Jump |
| X / Square | R | Reload |
| Menu / Options | Esc | KovaaK's menu |
| LS / L3 press | Shift | Sprint |
| Left stick | WASD | Movement (threshold 0.4, see `move_threshold`) |
| Right stick | Camera | Direct camera-model output |

Layout (Xbox vs PlayStation) is auto-selected by device VID; if it doesn't
match, run `--diag` to see the actual usage IDs and override them in
`config.json`:

```json
"buttons": { "fire": 6, "ads": 5, "jump": 1, "reload": 3, "escape": 8, "sprint": 9 }
```

## Configuration (config.json, next to the exe)

| Key | Default | Description |
|---|---|---|
| `loop_hz` | 16000 | Injection loop frequency (8000 saves CPU) |
| `settings_path` | auto | KovaaK's PrimaryUserSettings.json path (auto-detects C:/D: Steam libraries) |
| `deg_per_count_override` | 0 | When >0, fixes the conversion instead of reading game settings |
| `always_inject` | false | Disable the foreground gate (by default injection only happens while KovaaK's is focused) |
| `invert_y` | false | Invert pitch (the in-game YInvert is also honored) |
| `move_threshold` | 0.4 | Left stick → WASD trigger threshold (with hysteresis) |
| `apex.*` | see table above | Camera model params: look_curve / hip_aim_speed / ads_aim_speed / ads_scalar / deadzone / pitch_ratio |
| `target_processes` | KovaaK's | Foreground gate process names |

**Settings hot-reload**: after you change sensitivity inside KovaaK's, the
bridge re-reads and recomputes the conversion automatically (stick → on-screen
turn speed stays constant regardless of game sensitivity).

## Build

```powershell
powershell -File build.ps1   # w64devkit MinGW g++ (GCC 14)
```

Sources: `src/rawinput_reader.*` (HID report parsing and device management),
`src/apex_sens.h` (camera control model), `src/kovaak_config.*` (settings
derivation + expression evaluation), `src/injector.h` (SendInput),
`src/main.cpp` (16000Hz main loop / gate / mapping / self-test).

## Notes

1. **Disable Steam Input** (important): Steam → FPSAimTrainer → Properties →
   Controller → "Disable Steam Input", otherwise Steam intercepts the gamepad
   first and converts it to mouse/keyboard, causing double input.
2. **Matching privileges**: if KovaaK's runs as administrator, this tool must
   too (UIPI blocks lower-privilege injection).
3. **ADS scaling**: KovaaK's weapons apply their own FOV zoom when aiming, so
   the on-screen turning feel varies with FOV; the physical stick → °/s speed
   stays constant (that is the consistent definition of sensitivity). For
   identical on-screen feel, set KovaaK's FOV to match the target game
   (e.g. 110 horizontal for Apex).
4. 0 deadzone means stick drift passes straight through; if your stick drifts,
   set `apex.deadzone_in` in `config.json` (e.g. 0.02).
5. Measured: with a local controller (VID_413D, 8kHz polling), HID reads run
   at ~7990Hz with 16-bit resolution.
