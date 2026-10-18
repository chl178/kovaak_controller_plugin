// rawinput_reader.h — Win32 Raw Input HID 手柄读取器。
//
// APEX SIMULATOR gdext_rawinput/src/rawinput_reader.cpp 的独立移植：
//   - RIDEV_INPUTSINK 后台接收（不需要焦点，KovaaK's 全屏时照常工作）
//   - 独立消息线程逐报告解析，摇杆以原生 16-bit 分辨率归一化为 double
//     （1 计数 = 1/32768 摆幅，XInput 绕过、无死区、无滤波）
//   - 轴语义与模拟器一致：X/Y = 左摇杆；右摇杆优先 Rx/Ry，回退 Z/Rz（Xbox 约定）
//   - 每报告 QPC 时间戳 → 实测轮询率 / 最小步长 / 位深诊断
// 新增（模拟器中按钮走 Godot/SDL，此处必须自带）：
//   - HID 按钮（Usage Page 0x09）边沿解析 → 回调
//   - WM_INPUT_DEVICE_CHANGE 连接/断连通知 → 回调
//   - 每设备 preparsed data 缓存（避免每报告重复解析描述符）
#pragma once

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

class RawInputReader {
public:
	// 按钮_usage 边沿回调（读取线程内调用，必须快速返回）
	using ButtonCb = std::function<void(uint16_t usage, bool down)>;
	// 左摇杆报告回调（读取线程内调用；lx/ly ∈ [-1,1]）
	using StickCb = std::function<void(double lx, double ly)>;
	// 设备连接/断连回调（读取线程内调用；name = HID 设备路径）
	using DeviceCb = std::function<void(bool connected, const std::string &device_path)>;

	bool start(ButtonCb on_button, StickCb on_left_stick, DeviceCb on_device);
	void stop();
	bool is_started() const { return started_.load(); }
	bool is_active() const { return connected_.load(); }

	// 最新摇杆状态（右摇杆给瞄准主循环；左摇杆给移动，两者同源）
	void get_axes(double &lx, double &ly, double &rx, double &ry) const {
		std::lock_guard<std::mutex> lock(mtx_);
		lx = axes_[0].norm;
		ly = axes_[1].norm;
		rx = axes_[2].norm;
		ry = axes_[3].norm;
	}
	void get_raw(int64_t out[4]) const {
		std::lock_guard<std::mutex> lock(mtx_);
		for (int i = 0; i < 4; ++i) out[i] = axes_[i].raw;
	}
	double polling_rate() const {
		std::lock_guard<std::mutex> lock(mtx_);
		return prune_and_rate();
	}
	double min_step() const {
		std::lock_guard<std::mutex> lock(mtx_);
		return min_step_;
	}
	int bit_depth() const {
		std::lock_guard<std::mutex> lock(mtx_);
		return bit_depth_from_min_step(min_step_);
	}
	std::vector<uint16_t> pressed_buttons() const {
		std::lock_guard<std::mutex> lock(mtx_);
		return std::vector<uint16_t>(pressed_buttons_.begin(), pressed_buttons_.end());
	}
	std::string device_path() const {
		std::lock_guard<std::mutex> lock(mtx_);
		return device_path_;
	}

	static double normalize_count(int64_t count, double center, double half_range) {
		if (half_range <= 0.0) return 0.0;
		double v = (double(count) - center) / half_range;
		if (v > 1.0) v = 1.0;
		if (v < -1.0) v = -1.0;
		return v;
	}
	static int bit_depth_from_min_step(double min_step) {
		if (min_step <= 0.0 || min_step >= 1.0) return 0;
		double bits = std::log2(2.0 / min_step);
		double r = bits < 0.0 ? 0.0 : (bits > 16.0 ? 16.0 : bits);
		return int(r + 0.5);
	}

	// 窗口过程需要被匿名命名空间中的静态回调调用
	LRESULT wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

private:
	struct Axis {
		int64_t raw = 0;
		double norm = 0.0;
	};
	struct DevCache {
		std::vector<uint8_t> prep;
		bool valid = false;
	};

	void thread_main();
	void setup_message_window();
	void handle_wm_input(HRAWINPUT raw);
	void handle_device_change(WPARAM wparam, HANDLE device);
	bool ensure_cache(HANDLE device);
	void update_axes(const int64_t *counts, const double *center, const double *half, const bool *found,
			HANDLE device);
	void update_buttons(const std::vector<uint16_t> &pressed);
	void record_rate_sample(double now);
	double prune_and_rate() const; // 调用方需持 mtx_

	static double qpc_seconds();

	ButtonCb on_button_;
	StickCb on_stick_;
	DeviceCb on_device_;

	std::thread thread_;
	std::atomic<bool> running_{false};
	std::atomic<bool> started_{false};
	std::atomic<bool> connected_{false};
	std::atomic<HWND> hwnd_{nullptr};

	mutable std::mutex mtx_;
	Axis axes_[4];
	double last_norm_[4] = {0, 0, 0, 0};
	bool has_last_[4] = {false, false, false, false};
	double min_step_ = 1.0;
	std::vector<double> rate_ts_;
	std::string device_path_;
	HANDLE active_device_ = nullptr;
	std::map<HANDLE, DevCache> cache_;
	std::set<uint16_t> pressed_buttons_;
};
