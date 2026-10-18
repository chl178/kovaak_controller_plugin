// apex_sens.h — APEX SIMULATOR (controlsphere) 灵敏度数学的 1:1 C++ 移植。
//
// 来源（逐行对照）：
//   godot/scripts/player_controller.gd :: apex_controller_aim_64()
//   godot/scripts/sensitivity_engine.gd :: apply_response_curve()
//
// 用户 Apex 配置（必须保持一致）：
//   曲线 Linear(2) | Hip 4 = 255°/s | ADS 3 = 191.25°/s
//   死区 0%/0% | 俯仰 = 偏航 × 0.8 | 硬上限 510°/s (85 RPM)
//
// 数学全程使用 double（与模拟器 64-bit 摇杆采样管线一致），
// 1 个摇杆计数（1/32768 摆幅）不会被浮点截断丢失。
#pragma once

#include <algorithm>
#include <cmath>

struct ApexSens {
	// === APEX 常量（sensitivity_engine.gd / player_controller.gd 原值）===
	static constexpr double MAX_ROTATION_DEG_PER_SEC = 510.0; // 85 RPM
	static constexpr double DEG_PER_AIM_LEVEL = 63.75;        // 510 / 8
	static constexpr double PITCH_RATIO = 0.8;                // 俯仰 = 偏航 × 80%

	// === 用户配置（默认 = 用户 Apex profile.cfg 实际值）===
	int look_curve = 2;        // 0=Classic 1=Steady 2=Linear 3=Wide 4=Precise
	int hip_aim_speed = 4;     // 4 → 255.0 °/s
	int ads_aim_speed = 3;     // 3 → 191.25 °/s（用户所有倍镜速度均为 3）
	double ads_scalar = 1.0;   // 用户所有倍镜 scalar 均为 1.0
	double deadzone_in = 0.0;  // 0% 内死区（用户配置）
	double deadzone_out = 0.0; // 0% 外死区（用户配置）
	double pitch_ratio = PITCH_RATIO; // 俯仰 = 偏航 × 80%
	double speed_scale = 1.0;  // 整体转速校准系数（1.0 = 与模拟器/Apex 基准表一致）

	// 运行时状态
	bool is_ads = false;

	// 曲线：apply_response_curve() 的移植。
	// 所有曲线过 (0,0) 与 (1,1)；满杆时输出 1.0 = 满速。
	double apply_response_curve(double input) const {
		if (input <= 0.0) return 0.0;
		if (input >= 1.0) return 1.0;
		switch (look_curve) {
		case 0: return input * input * (3.0 - 2.0 * input); // Classic（Hermite 平滑）
		case 1: return std::pow(input, 2.5);                 // Steady
		case 2: return input;                                // Linear ← 用户
		case 3: return std::pow(input, 1.4);                 // Wide
		case 4: return std::pow(input, 1.8);                 // Precise
		default: return input;
		}
	}

	// 当前状态的最大转速（°/s），含 ADS scalar 与 510 硬上限。
	double max_speed() const {
		double base = double(is_ads ? ads_aim_speed : hip_aim_speed) * DEG_PER_AIM_LEVEL;
		if (is_ads) base *= ads_scalar;
		return std::min(base, MAX_ROTATION_DEG_PER_SEC);
	}

	// apex_controller_aim_64() 的移植：摇杆 (-1..1) × dt → 本周期旋转角（度）。
	// rx 右为正；ry 下为正（HID/Godot 约定）。返回 yaw_deg 右转为正、
	// pitch_deg 下视为正（与鼠标计数方向一致）。
	void aim(double rx, double ry, double dt, double *yaw_deg, double *pitch_deg) const {
		*yaw_deg = 0.0;
		*pitch_deg = 0.0;

		// 1. 幅度（圆形），钳到 1
		double magnitude = std::sqrt(rx * rx + ry * ry);
		if (magnitude > 1.0) magnitude = 1.0;
		if (magnitude == 0.0) return;

		// 2. 死区（用户 0/0 → 直通；公式与 player_controller.gd 相同）
		double dz_magnitude = magnitude;
		if (deadzone_in > 0.0 || deadzone_out > 0.0) {
			dz_magnitude = (magnitude - deadzone_in) / (1.0 - deadzone_out - deadzone_in);
			dz_magnitude = std::min(1.0, std::max(0.0, dz_magnitude));
		}
		if (dz_magnitude <= 0.0) return;

		// 3. 响应曲线（Linear = 恒等）
		double curved = apply_response_curve(dz_magnitude);

		// 4+5. 最终速度（speed_scale = 实测校准系数，见 config.json）
		double speed = curved * max_speed() * speed_scale;

		// 6. 方向（原始摇杆向量归一化）
		double inv_len = 1.0 / std::sqrt(rx * rx + ry * ry);
		double dx = rx * inv_len;
		double dy = ry * inv_len;

		// 俯仰 = 偏航 × pitch_ratio（默认 80%）
		*yaw_deg = dx * speed * dt;
		*pitch_deg = dy * speed * dt * pitch_ratio;
	}
};

// 摇杆计数 → 角速度（°/s）诊断辅助：与模拟器 get_max_speed_for_aim_level 相同。
inline double apex_max_speed_for_level(int aim_level) {
	return double(aim_level) * (ApexSens::MAX_ROTATION_DEG_PER_SEC / 8.0);
}
