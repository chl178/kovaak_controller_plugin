// rawinput_reader.cpp — 见头文件说明。轴解析逻辑与 APEX SIMULATOR
// gdext_rawinput/src/rawinput_reader.cpp 逐段对照移植。
#include "rawinput_reader.h"

#include <hidusage.h>
extern "C" {
#include <hidsdi.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr uint16_t USAGE_X = 0x30;  // 左摇杆 X
constexpr uint16_t USAGE_Y = 0x31;  // 左摇杆 Y
constexpr uint16_t USAGE_Z = 0x32;  // 右摇杆 X（Xbox 约定，回退）
constexpr uint16_t USAGE_RX = 0x33; // 右摇杆 X（优先；许多 HID 手柄）
constexpr uint16_t USAGE_RY = 0x34; // 右摇杆 Y（优先）
constexpr uint16_t USAGE_RZ = 0x35; // 右摇杆 Y（Xbox 约定，回退）
constexpr uint16_t USAGE_PAGE_BUTTON = 0x09;
constexpr wchar_t kWindowClassName[] = L"KovaakCtrlRawInputWnd";
constexpr double kNoiseFloor = 1e-12;
constexpr USHORT kMaxButtonUsages = 64;

LRESULT CALLBACK rawinput_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	RawInputReader *self = reinterpret_cast<RawInputReader *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	if (self != nullptr) {
		return self->wnd_proc(hwnd, msg, wparam, lparam);
	}
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

bool RawInputReader::start(ButtonCb on_button, StickCb on_stick, DeviceCb on_device) {
	std::lock_guard<std::mutex> lock(mtx_);
	if (started_.load()) return true;
	on_button_ = std::move(on_button);
	on_stick_ = std::move(on_stick);
	on_device_ = std::move(on_device);
	running_.store(true);
	thread_ = std::thread([this] { thread_main(); });
	started_.store(true);
	return true;
}

void RawInputReader::stop() {
	running_.store(false);
	HWND hwnd = hwnd_.load();
	if (hwnd != nullptr) {
		PostMessageW(hwnd, WM_QUIT, 0, 0);
	}
	if (thread_.joinable()) {
		thread_.join();
	}
	std::lock_guard<std::mutex> lock(mtx_);
	started_.store(false);
}

void RawInputReader::thread_main() {
	setup_message_window();
	HWND hwnd = hwnd_.load();
	if (hwnd == nullptr) {
		running_.store(false);
		return;
	}
	MSG msg;
	while (running_.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	HWND to_destroy = hwnd_.exchange(nullptr);
	if (to_destroy != nullptr) {
		DestroyWindow(to_destroy);
	}
	UnregisterClassW(kWindowClassName, GetModuleHandleW(nullptr));
	running_.store(false);
}

void RawInputReader::setup_message_window() {
	HINSTANCE inst = GetModuleHandleW(nullptr);
	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = rawinput_wnd_proc;
	wc.hInstance = inst;
	wc.lpszClassName = kWindowClassName;
	if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
		return;
	}
	HWND hwnd = CreateWindowExW(0, kWindowClassName, L"KovaakCtrlRawInput", 0,
			0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
	if (hwnd == nullptr) {
		return;
	}
	SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
	hwnd_.store(hwnd);

	RAWINPUTDEVICE rids[2]{};
	rids[0].usUsagePage = 0x01; // Generic Desktop
	rids[0].usUsage = 0x05;     // Game pad
	rids[0].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
	rids[0].hwndTarget = hwnd;
	rids[1].usUsagePage = 0x01;
	rids[1].usUsage = 0x08; // Multi-axis controller
	rids[1].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
	rids[1].hwndTarget = hwnd;
	if (!RegisterRawInputDevices(rids, 2, sizeof(RAWINPUTDEVICE))) {
		PostMessageW(hwnd, WM_QUIT, 0, 0);
	}
}

LRESULT RawInputReader::wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
	switch (msg) {
	case WM_INPUT:
		handle_wm_input(reinterpret_cast<HRAWINPUT>(lparam));
		return 0;
	case WM_INPUT_DEVICE_CHANGE:
		handle_device_change(wparam, reinterpret_cast<HANDLE>(lparam));
		return 0;
	default:
		return DefWindowProcW(hwnd, msg, wparam, lparam);
	}
}

void RawInputReader::handle_device_change(WPARAM wparam, HANDLE device) {
		if (wparam == GIDC_REMOVAL) {
		bool was_active = false;
		std::vector<uint16_t> release_usages;
		{
			std::lock_guard<std::mutex> lock(mtx_);
			cache_.erase(device);
			if (device == active_device_) {
				was_active = true;
				active_device_ = nullptr;
				connected_.store(false);
				for (int i = 0; i < 4; ++i) {
					axes_[i] = Axis{};
					last_norm_[i] = 0.0;
					has_last_[i] = false;
				}
				// 断连必须补发释放边沿 + 摇杆归零，否则注入的键/按钮卡死
				for (uint16_t u : pressed_buttons_) release_usages.push_back(u);
				pressed_buttons_.clear();
			}
		}
		if (was_active) {
			DeviceCb dcb;
			ButtonCb bcb;
			StickCb scb;
			{
				std::lock_guard<std::mutex> lock(mtx_);
				dcb = on_device_;
				bcb = on_button_;
				scb = on_stick_;
			}
			if (bcb) {
				for (uint16_t u : release_usages) bcb(u, false);
			}
			if (scb) scb(0.0, 0.0);
			if (dcb) dcb(false, "");
		}
	}
	// GIDC_ARRIVAL：设备出现无需处理，首个报告到达时自然激活。
}

bool RawInputReader::ensure_cache(HANDLE device) {
	// 调用方位于读取线程（单线程串行处理报告），直接查缓存
	auto it = cache_.find(device);
	if (it != cache_.end()) return it->second.valid;

	DevCache dc;
	UINT prep_size = 0;
	if (GetRawInputDeviceInfoW(device, RIDI_PREPARSEDDATA, nullptr, &prep_size) != UINT(-1) && prep_size > 0) {
		dc.prep.resize(prep_size);
		if (GetRawInputDeviceInfoW(device, RIDI_PREPARSEDDATA, dc.prep.data(), &prep_size) != UINT(-1)) {
			dc.valid = true;
		}
	}
	bool ok = dc.valid;
	cache_[device] = std::move(dc);
	return ok;
}

void RawInputReader::handle_wm_input(HRAWINPUT raw) {
	UINT size = 0;
	if (GetRawInputData(raw, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) == UINT(-1) || size == 0) {
		return;
	}
	std::vector<uint8_t> buffer(size);
	if (GetRawInputData(raw, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) {
		return;
	}
	RAWINPUT *ri = reinterpret_cast<RAWINPUT *>(buffer.data());
	if (ri->header.dwType != RIM_TYPEHID) {
		return;
	}
	HANDLE device = ri->header.hDevice;
	if (device == nullptr || !ensure_cache(device)) {
		return;
	}
	DevCache &dc = cache_[device];
	PHIDP_PREPARSED_DATA prep = reinterpret_cast<PHIDP_PREPARSED_DATA>(dc.prep.data());

	HIDP_CAPS caps{};
	if (HidP_GetCaps(prep, &caps) != HIDP_STATUS_SUCCESS) {
		return;
	}

	// ---- 轴（与 gdext_rawinput 相同的槽位优先级）----
	int64_t counts[4] = {0, 0, 0, 0};
	double center[4] = {0.0, 0.0, 0.0, 0.0};
	double half[4] = {1.0, 1.0, 1.0, 1.0};
	bool found[4] = {false, false, false, false};

	if (caps.NumberInputValueCaps > 0) {
		std::vector<HIDP_VALUE_CAPS> value_caps(caps.NumberInputValueCaps);
		USHORT caps_count = caps.NumberInputValueCaps;
		if (HidP_GetValueCaps(HidP_Input, value_caps.data(), &caps_count, prep) == HIDP_STATUS_SUCCESS) {
			for (const HIDP_VALUE_CAPS &vc : value_caps) {
				if (vc.UsagePage != 0x01 || vc.IsRange) {
					continue;
				}
				USAGE usage = vc.NotRange.Usage;
				int slot = -1;
				if (usage == USAGE_X) {
					slot = 0;
				} else if (usage == USAGE_Y) {
					slot = 1;
				} else if (usage == USAGE_RX) {
					slot = 2; // 优先：许多 HID 手柄右摇杆 = Rx/Ry
				} else if (usage == USAGE_RY) {
					slot = 3;
				} else if (usage == USAGE_Z && !found[2]) {
					slot = 2; // 回退：Xbox Z/Rz
				} else if (usage == USAGE_RZ && !found[3]) {
					slot = 3;
				}
				if (slot < 0) {
					continue;
				}
				ULONG value = 0;
				NTSTATUS status = HidP_GetUsageValue(HidP_Input, vc.UsagePage, vc.LinkCollection, usage,
						&value, prep, reinterpret_cast<PCHAR>(ri->data.hid.bRawData), ri->data.hid.dwSizeHid);
				if (status != HIDP_STATUS_SUCCESS) {
					continue;
				}
				counts[slot] = int64_t(value);
				double span = double(vc.LogicalMax) - double(vc.LogicalMin);
				if (span > 0.0) {
					center[slot] = double(vc.LogicalMin) + span * 0.5;
					half[slot] = span * 0.5;
				} else {
					// 退化 caps：按位宽的无符号约定（= XInput 映射，16-bit 时 1 计数 = 1/32768）
					double range = vc.BitSize >= 32 ? 4294967296.0 : double(int64_t(1) << vc.BitSize);
					center[slot] = range * 0.5;
					half[slot] = range * 0.5;
				}
				found[slot] = true;
			}
		}
	}

	if (found[0] || found[1] || found[2] || found[3]) {
		update_axes(counts, center, half, found, device);
	}

	// ---- 按钮（Usage Page 0x09）----
	std::vector<uint16_t> pressed;
	USAGE usage_list[kMaxButtonUsages];
	ULONG usage_len = kMaxButtonUsages;
	NTSTATUS bstatus = HidP_GetUsages(HidP_Input, USAGE_PAGE_BUTTON, 0, usage_list, &usage_len, prep,
			reinterpret_cast<PCHAR>(ri->data.hid.bRawData), ri->data.hid.dwSizeHid);
	if (bstatus == HIDP_STATUS_SUCCESS) {
		pressed.reserve(usage_len);
		for (ULONG i = 0; i < usage_len; ++i) {
			pressed.push_back(uint16_t(usage_list[i]));
		}
		std::sort(pressed.begin(), pressed.end());
		update_buttons(pressed);
	} else {
		update_buttons({});
	}
}

void RawInputReader::update_axes(const int64_t *counts, const double *center, const double *half,
		const bool *found, HANDLE device) {
	double norms[4] = {0.0, 0.0, 0.0, 0.0};
	double lx = 0.0, ly = 0.0;
	bool first_report = false;
	std::string path_copy;
	{
		std::lock_guard<std::mutex> lock(mtx_);
		for (int i = 0; i < 4; ++i) {
			if (!found[i]) continue;
			double norm = normalize_count(counts[i], center[i], half[i]);
			norms[i] = norm;
			axes_[i].raw = counts[i];
			axes_[i].norm = norm;
		}
		if (active_device_ != device) {
			active_device_ = device;
			if (!connected_.load()) first_report = true;
			connected_.store(true);
			UINT name_size = 0;
			std::string path;
			if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr, &name_size) != UINT(-1) && name_size > 0) {
				std::vector<wchar_t> nb(name_size);
				if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nb.data(), &name_size) != UINT(-1)) {
					int n = WideCharToMultiByte(CP_UTF8, 0, nb.data(), -1, nullptr, 0, nullptr, nullptr);
					if (n > 0) {
						std::vector<char> ub(static_cast<size_t>(n));
						WideCharToMultiByte(CP_UTF8, 0, nb.data(), -1, ub.data(), n, nullptr, nullptr);
						path.assign(ub.data());
					}
				}
			}
			device_path_ = path;
			path_copy = path;
		}
		// 最小非零步长（1 计数）诊断
		for (int i = 0; i < 4; ++i) {
			if (!found[i]) continue;
			if (has_last_[i]) {
				double d = std::abs(norms[i] - last_norm_[i]);
				if (d > kNoiseFloor && d < min_step_) {
					min_step_ = d;
				}
			}
			last_norm_[i] = norms[i];
			has_last_[i] = true;
		}
		record_rate_sample(qpc_seconds());
		lx = axes_[0].norm;
		ly = axes_[1].norm;
	}
	// 回调放锁外
	if (first_report && on_device_) {
		on_device_(true, path_copy);
	}
	if (on_stick_) {
		on_stick_(lx, ly);
	}
}

void RawInputReader::update_buttons(const std::vector<uint16_t> &pressed) {
	std::vector<std::pair<uint16_t, bool>> edges;
	ButtonCb cb;
	{
		std::lock_guard<std::mutex> lock(mtx_);
		cb = on_button_;
		std::set<uint16_t> now_set(pressed.begin(), pressed.end());
		for (uint16_t u : now_set) {
			if (pressed_buttons_.count(u) == 0) edges.emplace_back(u, true);
		}
		for (uint16_t u : pressed_buttons_) {
			if (now_set.count(u) == 0) edges.emplace_back(u, false);
		}
		pressed_buttons_ = std::move(now_set);
	}
	if (cb) {
		for (auto &e : edges) cb(e.first, e.second);
	}
}

void RawInputReader::record_rate_sample(double now) {
	rate_ts_.push_back(now);
	if (rate_ts_.size() > 4096) {
		rate_ts_.erase(rate_ts_.begin(), rate_ts_.end() - 1024);
	}
}

double RawInputReader::prune_and_rate() const {
	if (rate_ts_.size() < 2) return 0.0;
	double now = qpc_seconds();
	double cutoff = now - 1.0;
	size_t first = 0;
	while (rate_ts_.size() - first > 1 && rate_ts_[first] < cutoff) {
		++first;
	}
	size_t n = rate_ts_.size() - first;
	if (n < 2) return 0.0;
	double span = rate_ts_.back() - rate_ts_[first];
	if (span <= 0.0) return 0.0;
	return double(n - 1) / span;
}

double RawInputReader::qpc_seconds() {
	LARGE_INTEGER freq, now;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	return double(now.QuadPart) / double(freq.QuadPart);
}
