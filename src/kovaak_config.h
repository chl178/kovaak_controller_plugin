// kovaak_config.h — 读取 KovaaK's 用户灵敏度设置，得到"每鼠标计数的旋转角度"。
//
// 文件：
//   PrimaryUserSettings.json — floatSettings["XSens"/"YSens"]、
//       stringSettings["SensScaleString"]、integerSettings["DPI"]、floatSettings["FOV"]
//   FovSensConfig.json       — 各游戏刻度表（ScaleName → Sens.IncrementFormula）
//
// 例：用户 CS 刻度 IncrementFormula "Sens * 0.022"，XSens=1.8
//     → deg_per_count_x = 1.8 × 0.022 = 0.0396°/count
//
// 注入时 counts/s = 目标°/s ÷ deg_per_count，与 KovaaK's 当前灵敏度解耦：
// 游戏内改灵敏度，本插件自动换算，摇杆→屏幕旋转速度始终与 APEX SIMULATOR 1:1。
#pragma once

#include <string>

struct KovaakSens {
	bool ok = false;            // 是否成功从真实设置推导
	double deg_per_count_x = 0.022; // X 轴（偏航）每计数角度，默认 Source 0.022
	double deg_per_count_y = 0.022; // Y 轴（俯仰）每计数角度
	double xsens = 0.0;
	double ysens = 0.0;
	double fov = 100.0;
	int dpi = 800;
	bool y_invert = false;
	std::string scale_name;     // 如 "Counter-Strike"
	std::string source_path;
	std::string note;           // 人类可读推导说明（或降级原因）

	double cm360(int dpi_override = 0) const {
		double d = dpi_override > 0 ? double(dpi_override) : double(dpi);
		if (deg_per_count_x <= 0 || d <= 0) return 0.0;
		return (360.0 * 2.54) / (d * deg_per_count_x);
	}
};

// 读取并计算。settings_path 为 PrimaryUserSettings.json 完整路径；
// FovSensConfig.json 必须与其同目录。失败时字段回退默认并写明 note。
KovaakSens kovaak_load_sens(const std::wstring &settings_path);
