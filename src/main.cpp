// main.cpp — kovaak_controller：APEX SIMULATOR 手柄输入的 KovaaK's 桥。
//
// 输入侧 100% 原生：RawInput HID 直读手柄（16-bit 轴、0 死区、双精度、
// 设备报告率），不经 XInput/Steam Input/摇杆→光标转换层。
// 灵敏度 100% 同源：APEX SIMULATOR player_controller.apex_controller_aim_64()
// 的 1:1 移植（Linear、Hip 4=255°/s、ADS 3=191.25°/s、俯仰 0.8、上限 510°/s）。
// 延迟同级：默认 16000Hz QPC 主循环采样最新摇杆状态并立即注入
// （对应模拟器 Engine.max_fps=16000 的逐帧采样语义）。
// 输出：°/s ÷ KovaaK's deg/count（读 PrimaryUserSettings.json 自动推导）
// → SendInput 相对鼠标计数（KovaaK's 唯一可用的输入通道，等价真实鼠标计数）。
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "apex_sens.h"
#include "injector.h"
#include "expr_eval.h"
#include "json_min.h"
#include "kovaak_config.h"
#include "rawinput_reader.h"

// ============================================================
// 配置（config.json 可覆盖；默认 = 用户 Apex 配置 + 本机 KovaaK 路径）
// ============================================================
struct Config {
	int loop_hz = 16000;
	std::wstring settings_path;
	double deg_per_count_override = 0.0;
	bool always_inject = false;
	bool invert_y = false;
	bool no_move = false;
	double move_threshold = 0.4;
	std::vector<std::string> targets; // 前台进程名（小写）
	ApexSens apex;                    // 默认 = 用户 Apex profile
	// 按键映射（HID usage ID；0=禁用；空 = 按设备自动选择）
	struct ButtonMap {
		int fire = -1, ads = -1, jump = -1, reload = -1, escape = -1, sprint = -1;
	} buttons;

	Config() {
		targets = {"fpsaimtrainer-win64-shipping.exe", "fpsaimtrainer.exe"};
	}
};

namespace {

constexpr int kVkW = 0x57, kVkA = 0x41, kVkS = 0x53, kVkD = 0x44;
constexpr int kVkSpace = 0x20, kVkR = 0x52, kVkEsc = 0x1B, kVkShift = 0x10;

std::atomic<bool> g_shutdown{false};

BOOL WINAPI console_ctrl_handler(DWORD type) {
	if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
		g_shutdown.store(true);
		return TRUE;
	}
	return FALSE;
}

std::string utf8_ws(const std::wstring &w) {
	if (w.empty()) return "";
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
	std::string out(size_t(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), out.data(), n, nullptr, nullptr);
	return out;
}
std::wstring utf8_sw(const std::string &s) {
	if (s.empty()) return L"";
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
	std::wstring out(size_t(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), out.data(), n);
	return out;
}
std::string to_lower(std::string s) {
	for (char &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
	return s;
}
bool ends_with_ci(const std::wstring &s, const std::string &suffix) {
	std::wstring w = utf8_sw(suffix);
	if (w.size() > s.size()) return false;
	return std::equal(w.begin(), w.end(), s.end() - w.size(), [](wchar_t a, wchar_t b) {
		return std::towlower(a) == std::towlower(b);
	});
}

// exe 所在目录
std::wstring exe_dir() {
	wchar_t buf[MAX_PATH];
	DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
	std::wstring p(buf, n);
	size_t slash = p.find_last_of(L"\\/");
	return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

// 自动发现 KovaaK's PrimaryUserSettings.json（常见 Steam 库位置）
std::wstring autodetect_settings() {
	const wchar_t *candidates[] = {
		L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\FPSAimTrainer\\FPSAimTrainer\\Saved\\SaveGames\\PrimaryUserSettings.json",
		L"D:\\SteamLibrary\\steamapps\\common\\FPSAimTrainer\\FPSAimTrainer\\Saved\\SaveGames\\PrimaryUserSettings.json",
	};
	for (const wchar_t *c : candidates) {
		if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) return c;
	}
	// Steam libraryfolders.vdf 扫描（两级展开）
	return L"";
}

// ---------------- config.json ----------------
void load_config_file(Config &cfg, const std::wstring &path) {
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;
	DWORD size = GetFileSize(h, nullptr);
	std::string text(size, '\0');
	DWORD rd = 0;
	ReadFile(h, text.data(), size, &rd, nullptr);
	CloseHandle(h);
	text.resize(rd);

	jmin::Ptr root;
	std::string err;
	if (!jmin::parse(text, root, err) || !root) {
		fprintf(stderr, "[config] %ls 解析失败: %s（使用默认值）\n", path.c_str(), err.c_str());
		return;
	}
	jmin::Ptr v;
	if ((v = root->get("loop_hz"))) cfg.loop_hz = int(v->num_or(cfg.loop_hz));
	if ((v = root->get("settings_path")) && v->type == jmin::Value::STR && !v->str.empty())
		cfg.settings_path = utf8_sw(v->str);
	if ((v = root->get("deg_per_count_override"))) cfg.deg_per_count_override = v->num_or(0.0);
	if ((v = root->get("always_inject"))) cfg.always_inject = v->bool_or(false);
	if ((v = root->get("invert_y"))) cfg.invert_y = v->bool_or(false);
	if ((v = root->get("no_move"))) cfg.no_move = v->bool_or(false);
	if ((v = root->get("move_threshold"))) cfg.move_threshold = v->num_or(0.4);
	if ((v = root->get("target_processes")) && v->type == jmin::Value::ARR) {
		cfg.targets.clear();
		for (auto &e : v->arr)
			if (e->type == jmin::Value::STR) cfg.targets.push_back(to_lower(e->str));
	}
	if ((v = root->get("apex")) && v->type == jmin::Value::OBJ) {
		jmin::Ptr a;
		if ((a = v->get("look_curve"))) cfg.apex.look_curve = int(a->num_or(2));
		if ((a = v->get("hip_aim_speed"))) cfg.apex.hip_aim_speed = int(a->num_or(4));
		if ((a = v->get("ads_aim_speed"))) cfg.apex.ads_aim_speed = int(a->num_or(3));
		if ((a = v->get("ads_scalar"))) cfg.apex.ads_scalar = a->num_or(1.0);
		if ((a = v->get("deadzone_in"))) cfg.apex.deadzone_in = a->num_or(0.0);
		if ((a = v->get("deadzone_out"))) cfg.apex.deadzone_out = a->num_or(0.0);
		if ((a = v->get("pitch_ratio"))) cfg.apex.pitch_ratio = a->num_or(0.8);
		if ((a = v->get("speed_scale"))) cfg.apex.speed_scale = a->num_or(1.0);
	}
	if ((v = root->get("buttons")) && v->type == jmin::Value::OBJ) {
		jmin::Ptr b;
		auto geti = [&](const char *k, int &dst) {
			if ((b = v->get(k))) dst = int(b->num_or(-1));
		};
		geti("fire", cfg.buttons.fire);
		geti("ads", cfg.buttons.ads);
		geti("jump", cfg.buttons.jump);
		geti("reload", cfg.buttons.reload);
		geti("escape", cfg.buttons.escape);
		geti("sprint", cfg.buttons.sprint);
	}
}

// ---------------- 前台闸门 ----------------
struct ForegroundGate {
	std::vector<std::string> targets;
	DWORD cached_pid = 0;
	bool cached_ok = false;

	bool is_target_foreground() {
		HWND fg = GetForegroundWindow();
		if (fg == nullptr) return false;
		DWORD pid = 0;
		GetWindowThreadProcessId(fg, &pid);
		if (pid == 0) return false;
		if (pid == cached_pid && cached_ok) return true; // 快路径
		bool ok = false;
		HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
		if (h != nullptr) {
			wchar_t buf[MAX_PATH];
			DWORD sz = MAX_PATH;
			if (QueryFullProcessImageNameW(h, 0, buf, &sz)) {
				std::wstring name(buf, sz);
				for (const auto &t : targets) {
					if (ends_with_ci(name, t)) {
						ok = true;
						break;
					}
				}
			}
			CloseHandle(h);
		}
		cached_pid = pid;
		cached_ok = ok;
		return ok;
	}
};

// ---------------- 按键映射 ----------------
// HID usage ID 默认表。Xbox（蓝牙 HID / 大多数第三方 XInput 手柄的 HID 面）：
//   1=A 2=B 3=X 4=Y 5=LB 6=RB 7=View 8=Menu 9=LS 10=RS
// PlayStation（DS4/DualSense HID 描述符）：
//   1=方块 2=叉 3=圆 4=三角 5=L1 6=R1 7=L2 8=R2 9=Share 10=Options 11=L3 12=R3
// 不确定时用 --diag 查看实机 usage ID，在 config.json buttons 里覆盖。
enum class PadLayout { Xbox, PlayStation };
const char *layout_name(PadLayout l) { return l == PadLayout::Xbox ? "Xbox" : "PlayStation"; }

PadLayout detect_layout(const std::string &device_path) {
	std::string p = to_lower(device_path);
	if (p.find("vid_054c") != std::string::npos) return PadLayout::PlayStation; // Sony
	return PadLayout::Xbox;
}

void apply_layout_defaults(Config::ButtonMap &bm, PadLayout l) {
	if (l == PadLayout::PlayStation) {
		bm.fire = 6;    // R1
		bm.ads = 5;     // L1
		bm.jump = 2;    // 叉
		bm.reload = 1;  // 方块
		bm.escape = 10; // Options
		bm.sprint = 11; // L3
	} else {
		bm.fire = 6;   // RB
		bm.ads = 5;    // LB
		bm.jump = 1;   // A
		bm.reload = 3; // X
		bm.escape = 8; // Menu
		bm.sprint = 9; // LS
	}
}

// ---------------- 共享运行时状态 ----------------
struct Runtime {
	RawInputReader reader;
	Injector injector;
	std::atomic<bool> ads{false};
	std::atomic<bool> gate_on{false};
	Config cfg;
	KovaakSens sens;
	std::atomic<uint64_t> last_move_count{0};

	// WASD 状态（读取线程写、闸门同步读；注入器内部串行化键序）
	bool wasd[4] = {false, false, false, false}; // W A S D
	std::mutex wasd_mtx;

	void set_wasd(int i, bool down) {
		std::lock_guard<std::mutex> lock(wasd_mtx);
		if (wasd[i] == down) return;
		wasd[i] = down;
		WORD vk = i == 0 ? kVkW : i == 1 ? kVkA : i == 2 ? kVkS : kVkD;
		injector.key(vk, down);
	}
	void clear_wasd() {
		for (int i = 0; i < 4; ++i) set_wasd(i, false);
	}
};

double qpc_now_s() {
	LARGE_INTEGER f, t;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	return double(t.QuadPart) / double(f.QuadPart);
}

void print_calibration(const Runtime &rt) {
	const Config &cfg = rt.cfg;
	const KovaakSens &s = rt.sens;
	const char *curve_names[] = {"Classic", "Steady", "Linear", "Wide", "Precise"};
	printf("=== kovaak_controller — 输入标定（与 APEX SIMULATOR 同源）===\n");
	printf("  曲线: %s (%d) | 死区: %.0f%%/%.0f%% | 俯仰比: %.2f\n",
			curve_names[std::clamp(cfg.apex.look_curve, 0, 4)], cfg.apex.look_curve,
			cfg.apex.deadzone_in * 100.0, cfg.apex.deadzone_out * 100.0, cfg.apex.pitch_ratio);
	printf("  Hip: %d (%.2f°/s) | ADS: %d (%.2f°/s) | 硬上限: %.0f°/s (85 RPM)\n",
			cfg.apex.hip_aim_speed, apex_max_speed_for_level(cfg.apex.hip_aim_speed),
			cfg.apex.ads_aim_speed, apex_max_speed_for_level(cfg.apex.ads_aim_speed) * cfg.apex.ads_scalar,
			ApexSens::MAX_ROTATION_DEG_PER_SEC);
	printf("  KovaaK's: %s\n", s.note.c_str());
	printf("  换算: %.6g°/count(X) %.6g°/count(Y) | cm/360 @%ddpi = %.2f | %s\n",
			s.deg_per_count_x, s.deg_per_count_y, s.dpi, s.cm360(), s.source_path.c_str());
	printf("  注入循环: %d Hz (QPC) | 满杆 Hip = %.0f°/s = %.0f counts/s\n",
			cfg.loop_hz, apex_max_speed_for_level(cfg.apex.hip_aim_speed),
			apex_max_speed_for_level(cfg.apex.hip_aim_speed) / s.deg_per_count_x);
	printf("=============================================================\n");
}

} // namespace

// ============================================================
// 瞄准主循环（默认 16000Hz，语义 = 模拟器 _process 逐帧采样）
// ============================================================
namespace {

void aim_loop(Runtime &rt) {
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	const double tick = 1.0 / double(std::max(1, rt.cfg.loop_hz));
	// 低频家务节奏（10Hz）
	const uint64_t housekeeping_every = std::max<uint64_t>(1, uint64_t(rt.cfg.loop_hz / 10));
	uint64_t tick_n = 0;

	// KovaaK 设置热重载
	FILETIME last_mtime{};
	WIN32_FILE_ATTRIBUTE_DATA fad{};
	std::wstring settings_path = rt.sens.source_path.empty() ? L"" : utf8_sw(rt.sens.source_path);
	if (GetFileAttributesExW(settings_path.c_str(), GetFileExInfoStandard, &fad)) last_mtime = fad.ftLastWriteTime;
	ForegroundGate gate;
	gate.targets = rt.cfg.targets;

	double acc_x = 0.0, acc_y = 0.0; // 小数计数累加器
	double next = qpc_now_s() + tick;
	double last = qpc_now_s();
	

	while (!g_shutdown.load()) {
		double now = qpc_now_s();
		while (now < next) {
			__builtin_ia32_pause();
			now = qpc_now_s();
		}
		double dt = now - last;
		last = now;
		next += tick;
		if (next < now - tick * 4.0) next = now + tick; // 卡顿后重同步

		// --- 采样最新摇杆（与模拟器每帧读最新 RawInput 状态一致）---
		if (rt.reader.is_active()) {
			double lx, ly, rx, ry;
			rt.reader.get_axes(lx, ly, rx, ry);
			ApexSens apex = rt.cfg.apex; // 热路径无锁拷贝（POD）
			apex.is_ads = rt.ads.load();
			double yaw_deg, pitch_deg;
			apex.aim(rx, ry, dt, &yaw_deg, &pitch_deg);
			if (rt.cfg.invert_y || rt.sens.y_invert) pitch_deg = -pitch_deg;
			acc_x += yaw_deg / rt.sens.deg_per_count_x;
			acc_y += pitch_deg / rt.sens.deg_per_count_y;
			int cx = int(acc_x);
			int cy = int(acc_y);
			if (cx != 0 || cy != 0) {
				acc_x -= cx;
				acc_y -= cy;
				rt.injector.mouse_move(cx, cy);
			}
		} else {
			acc_x = 0.0;
			acc_y = 0.0;
		}

		// --- 10Hz 家务：前台闸门 + 设置热重载 ---
		if (++tick_n % housekeeping_every == 0) {
			bool on = rt.cfg.always_inject || gate.is_target_foreground();
			if (on != rt.gate_on.load()) {
				rt.gate_on.store(on);
				rt.injector.set_enabled(on);
				printf("[闸门] %ls\n", on ? L"KovaaK's 前台 → 注入开启" : L"离开 KovaaK's → 注入暂停（按键已释放）");
				if (!on) {
					std::lock_guard<std::mutex> lock(rt.wasd_mtx);
					for (int i = 0; i < 4; ++i) rt.wasd[i] = false; // 注入器已 release_all
				}
			}
			if (GetFileAttributesExW(settings_path.c_str(), GetFileExInfoStandard, &fad)) {
				if (CompareFileTime(&fad.ftLastWriteTime, &last_mtime) != 0) {
					last_mtime = fad.ftLastWriteTime;
					if (rt.cfg.deg_per_count_override <= 0.0) {
						Sleep(150); // 等游戏写完
						KovaakSens s = kovaak_load_sens(settings_path);
						if (s.ok) {
							rt.sens = s;
							printf("[设置] KovaaK's 灵敏度已重载: %s (%.5g°/count)\n",
									s.note.c_str(), s.deg_per_count_x);
						}
					}
				}
			}
		}
	}
	rt.injector.release_all();
}

// ============================================================
// 手柄事件回调（读取线程内调用）
// ============================================================
void on_button(Runtime &rt, uint16_t usage, bool down) {
	const auto &bm = rt.cfg.buttons;
	if (int(usage) == bm.fire && bm.fire > 0) {
		rt.injector.mouse_button(true, down);
	} else if (int(usage) == bm.ads && bm.ads > 0) {
		rt.injector.mouse_button(false, down);
		rt.ads.store(down);
	} else if (int(usage) == bm.jump && bm.jump > 0) {
		rt.injector.key(kVkSpace, down);
	} else if (int(usage) == bm.reload && bm.reload > 0) {
		rt.injector.key(kVkR, down);
	} else if (int(usage) == bm.escape && bm.escape > 0) {
		if (down) rt.injector.key(kVkEsc, true);
		else rt.injector.key(kVkEsc, false);
	} else if (int(usage) == bm.sprint && bm.sprint > 0) {
		rt.injector.key(kVkShift, down);
	}
}

void on_left_stick(Runtime &rt, double lx, double ly) {
	if (rt.cfg.no_move) return;
	// 每轴独立阈值 + 迟滞（进入 thr，退出 thr-0.05）
	const double thr = rt.cfg.move_threshold;
	const double rel = thr - 0.05;
	auto axis = [&](double v, int neg_idx, int pos_idx) {
		bool neg_hold, pos_hold;
		{
			std::lock_guard<std::mutex> lock(rt.wasd_mtx);
			neg_hold = rt.wasd[neg_idx];
			pos_hold = rt.wasd[pos_idx];
		}
		bool want_neg = v < -(neg_hold ? rel : thr);
		bool want_pos = v > (pos_hold ? rel : thr);
		if (want_neg && want_pos) want_pos = false;
		if (neg_hold != want_neg) rt.set_wasd(neg_idx, want_neg);
		if (pos_hold != want_pos) rt.set_wasd(pos_idx, want_pos);
	};
	axis(lx, 1, 3); // A / D
	axis(ly, 0, 2); // W（上 = 负 Y）/ S
}

void on_device(Runtime &rt, bool connected, const std::string &path) {
	if (connected) {
		// 首次连接：按 VID 选默认按键映射（config.json 显式 buttons 优先）
		bool has_explicit = rt.cfg.buttons.fire > 0;
		if (!has_explicit) {
			PadLayout l = detect_layout(path);
			apply_layout_defaults(rt.cfg.buttons, l);
			printf("[手柄] 已连接: %s\n       按键布局: %s（不对请用 --diag 查 usage ID 并在 config.json 覆盖）\n",
					path.c_str(), layout_name(l));
		} else {
			printf("[手柄] 已连接: %s（使用 config.json 按键映射）\n", path.c_str());
		}
	} else {
		printf("[手柄] 断开：已释放全部注入按键\n");
		rt.clear_wasd();
		rt.ads.store(false);
		rt.injector.release_all();
	}
}

// ============================================================
// --diag：实机诊断（轴计数/按钮 usage/轮询率/位深）
// ============================================================
void diag_loop(Runtime &rt) {
	printf("--diag 模式：移动摇杆/按按钮查看实机 HID usage ID（Ctrl+C 退出）\n");
	uint64_t last_print = 0;
	while (!g_shutdown.load()) {
		uint64_t now_ms = GetTickCount64();
		if (now_ms - last_print >= 100 && rt.reader.is_active()) {
			last_print = now_ms;
			int64_t raw[4];
			rt.reader.get_raw(raw);
			double lx, ly, rx, ry;
			rt.reader.get_axes(lx, ly, rx, ry);
			std::vector<uint16_t> btns = rt.reader.pressed_buttons();
			std::string bl;
			for (uint16_t b : btns) bl += std::to_string(int(b)) + " ";
			printf("\rL(%6d,%6d) R(%6d,%6d) | R归一 (%+.5f,%+.5f) | 轮询 %.0fHz 位深%d | 按下usage: %-26s   ",
					int(raw[0]), int(raw[1]), int(raw[2]), int(raw[3]), rx, ry,
					rt.reader.polling_rate(), rt.reader.bit_depth(), bl.c_str());
			fflush(stdout);
		} else if (!rt.reader.is_active()) {
			printf("\r等待手柄 HID 报告...（确认 Steam Input 已对本游戏禁用）   ");
			fflush(stdout);
			Sleep(500);
		}
		Sleep(50);
	}
	printf("\n");
}

} // namespace

// ============================================================
// --selftest：数学 1:1 / 表达式 / JSON / 积分器 校验（无需手柄）
// ============================================================
namespace {

int g_fail = 0, g_pass = 0;
void check(bool ok, const char *name, double got = 0.0, double want = 0.0) {
	if (ok) {
		++g_pass;
		printf("  [PASS] %s\n", name);
	} else {
		++g_fail;
		printf("  [FAIL] %s (got %.17g want %.17g)\n", name, got, want);
	}
}
bool near_d(double a, double b, double eps = 1e-9) { return std::abs(a - b) <= eps; }

int run_selftest(const std::wstring &settings_path) {
	printf("=== selftest：APEX SIMULATOR 数学 1:1 校验 ===\n");

	// --- Apex 数学（golden 值来自 player_controller.gd / test_runner.gd）---
	{
		ApexSens ap; // 默认 = 用户配置：Linear 4-3, 0/0 死区
		double y, p;
		check(ap.max_speed() == 255.0, "hip 满速 = 255°/s", ap.max_speed(), 255.0);
		ap.is_ads = true;
		check(ap.max_speed() == 191.25, "ADS 满速 = 191.25°/s", ap.max_speed(), 191.25);
		ap.is_ads = false;

		ap.aim(1.0, 0.0, 1.0, &y, &p);
		check(y == 255.0 && p == 0.0, "满杆右 → 偏航 255°/s", y, 255.0);
		ap.aim(-1.0, 0.0, 1.0, &y, &p);
		check(y == -255.0, "满杆左 → -255°/s", y, -255.0);
		ap.aim(0.0, 1.0, 1.0, &y, &p);
		check(y == 0.0 && p == 204.0, "满杆下 → 俯仰 255×0.8 = 204°/s", p, 204.0);
		ap.aim(0.0, -1.0, 1.0, &y, &p);
		check(p == -204.0, "满杆上 → -204°/s", p, -204.0);
		ap.aim(0.6, 0.8, 1.0, &y, &p);
		check(near_d(y, 153.0) && near_d(p, 163.2), "对角 (0.6,0.8) 幅度1 → 153 / 163.2", y, 153.0);
		// 1 计数步进（16-bit 精度保持）
		double one = 1.0 / 32768.0;
		ap.aim(one, 0.0, 1.0, &y, &p);
		check(y == one * 255.0, "1 计数(1/32768) → 0.007781982421875°/s", y, one * 255.0);
		// 幅度钳制：对角 45° 幅度略超 1 → 1.0
		double d45 = 0.7071067811865476;
		ap.aim(d45, d45, 1.0, &y, &p);
		check(near_d(y, d45 * 255.0) && near_d(p, d45 * 204.0), "对角 45° 幅度钳 1.0", y, d45 * 255.0);
		// ADS
		ap.is_ads = true;
		ap.aim(1.0, 0.0, 1.0, &y, &p);
		check(y == 191.25, "ADS 满杆 → 191.25°/s", y, 191.25);
		ap.is_ads = false;
		// 死区公式（player_controller.gd：dz=(m-dzin)/(1-dzout-dzin)）
		ApexSens dz;
		dz.deadzone_in = 0.2;
		dz.aim(0.6, 0.0, 1.0, &y, &p);
		check(near_d(y, 127.5), "死区 20%：幅度 0.6 → dz 0.5 → 127.5°/s", y, 127.5);
		dz.aim(0.1, 0.0, 1.0, &y, &p);
		check(y == 0.0, "死区内 → 0", y, 0.0);
		// 曲线族
		check(near_d(ap.apply_response_curve(0.5), 0.5), "Linear(0.5)=0.5");
		ApexSens c;
		c.look_curve = 0;
		check(near_d(c.apply_response_curve(0.5), 0.5, 1e-12), "Classic(0.5)=0.5");
		c.look_curve = 1;
		check(near_d(c.apply_response_curve(0.5), 0.17677669529663687, 1e-12), "Steady(0.5)=0.5^2.5");
		c.look_curve = 3;
		check(near_d(c.apply_response_curve(0.5), 0.3789291416275996, 1e-12), "Wide(0.5)=0.5^1.4");
		c.look_curve = 4;
		check(near_d(c.apply_response_curve(0.5), 0.28717458874925863, 1e-12), "Precise(0.5)=0.5^1.8");
		// aim level 线性表
		check(apex_max_speed_for_level(1) == 63.75 && apex_max_speed_for_level(8) == 510.0,
				"aim level 表：1→63.75, 8→510");

		// 校准系数
		{
			ApexSens sc;
			sc.speed_scale = 0.9;
			double y2, p2;
			sc.aim(1.0, 0.0, 1.0, &y2, &p2);
			check(near_d(y2, 229.5), "speed_scale 0.9 → 229.5°/s", y2, 229.5);
		}
	}

	// --- 表达式求值（FovSensConfig IncrementFormula 形态）---
	{
		auto res = [](double sens) {
			// 按值捕获：外层参数随调用即销毁，按引用捕获会悬垂
			return [sens](const std::string &n) -> double {
				if (n == "Sens") return sens;
				if (n == "DPI") return 800.0;
				if (n == "FOV") return 100.0;
				if (n == "_pi") return 3.14159265358979323846;
				return std::nan("");
			};
		};
		check(near_d(ExprEval::eval("Sens * 0.022", res(1.8)), 0.0396, 1e-12), "CS 刻度 1.8×0.022=0.0396");
		check(near_d(ExprEval::eval("Sens * 0.104156 * FOV * 0.001", res(1.0)), 0.0104156, 1e-12),
				"Splitgate FOV 依赖公式");
		check(near_d(ExprEval::eval("0.00005555 * 10 ^ (Sens / 50) * FOV", res(50.0)), 0.05555, 1e-12),
				"PUBG 指数公式");
		check(near_d(ExprEval::eval("(Sens * 0.005446) + 0.032809", res(1.0)), 0.038255, 1e-12),
				"Battlefield 仿射公式");
		check(near_d(ExprEval::eval("Sens * 0.018 / _pi", res(1.0)), 0.005729577951308233, 1e-15),
				"R6S _pi 公式");
	}

	// --- JSON 解析 ---
	{
		jmin::Ptr v;
		std::string err;
		bool ok = jmin::parse("{\"floatSettings\":{\"EFloatSettingId::XSens\":1.7999999523162842},"
							  "\"list\":[1,2,{\"a\":true},null,\"\\u4e2d\\u6587\"]}",
				v, err);
		check(ok, "JSON 解析（嵌套/转义）");
		if (ok) {
			check(near_d(v->get("floatSettings")->get("EFloatSettingId::XSens")->num, 1.7999999523162842),
					"JSON 双精度数字保持");
			check(v->get("list")->arr[3]->type == jmin::Value::NUL, "JSON null");
			check(v->get("list")->arr[2]->get("a")->b == true, "JSON 嵌套布尔");
		}
	}

	// --- KovaaK 设置实读（若存在）---
	{
		std::wstring sp = settings_path;
		if (sp.empty()) sp = autodetect_settings();
		if (!sp.empty()) {
			KovaakSens s = kovaak_load_sens(sp);
			check(s.ok, "KovaaK's 设置推导 deg/count 成功");
			if (s.ok) {
				printf("        实测: %s | cm/360=%.2f\n", s.note.c_str(), s.cm360());
				// 用户当前 CS 刻度 XSens≈1.8（游戏以 float32 存储 → 0.03959999895…）
				check(near_d(s.deg_per_count_x, 0.0396, 1e-7), "XSens 1.8 × 0.022 ≈ 0.0396°/count",
						s.deg_per_count_x, 0.0396);
			}
		} else {
			printf("  [SKIP] 未找到 KovaaK's PrimaryUserSettings.json\n");
		}
	}

	// --- 积分器端到端：满杆 1 秒 → 计数速率 ---
	{
		ApexSens ap;
		const double deg = 0.0396;
		const int hz = 16000;
		const double dt = 1.0 / double(hz);
		double acc = 0.0;
		int64_t emitted = 0;
		for (int i = 0; i < hz; ++i) {
			double y, p;
			ap.aim(1.0, 0.0, dt, &y, &p);
			acc += y / deg;
			int c = int(acc);
			acc -= c;
			emitted += c;
		}
		// 255°/s ÷ 0.0396°/count = 6439.39 counts/s（±1 量化）
		check(emitted >= 6438 && emitted <= 6440, "满杆 Hip 1s → 6439±1 counts（255/0.0396）",
				double(emitted), 6439.3939);
	}

	// --- 归一化 / 位深（RawInputReader 静态数学）---
	{
		check(RawInputReader::normalize_count(32768, 32768.0, 32768.0) == 0.0, "中心 → 0");
		check(RawInputReader::normalize_count(32769, 32768.0, 32768.0) == 1.0 / 32768.0, "+1 计数 → 1/32768");
		check(RawInputReader::bit_depth_from_min_step(1.0 / 32768.0) == 16, "1/32768 → 16-bit");
	}

	printf("=== selftest 结果: %d 通过 / %d 失败 ===\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
	}

} // namespace

// ============================================================
// main
// ============================================================
int main(int argc, char **argv) {
	SetConsoleOutputCP(CP_UTF8);
	setvbuf(stdout, nullptr, _IOLBF, 0); // 管道/重定向时也逐行刷新
	SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

	Config cfg;
	std::wstring config_path = exe_dir() + L"\\config.json";
	bool selftest = false, diag = false, dry = false;

	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		auto next = [&](const char *def) -> std::string {
			return i + 1 < argc ? std::string(argv[++i]) : std::string(def);
		};
		if (a == "--selftest") selftest = true;
		else if (a == "--diag") diag = true;
		else if (a == "--dry-run") dry = true;
		else if (a == "--config") config_path = utf8_sw(next(""));
		else if (a == "--loop-hz") cfg.loop_hz = std::max(125, atoi(next("16000").c_str()));
		else if (a == "--settings") cfg.settings_path = utf8_sw(next(""));
		else if (a == "--always") cfg.always_inject = true;
		else if (a == "--help" || a == "-h") {
			printf("kovaak_controller — APEX SIMULATOR 手柄输入的 KovaaK's 桥\n\n"
				   "用法: kovaak_controller [--selftest] [--diag] [--dry-run] [--always]\n"
				   "      [--config <config.json>] [--settings <PrimaryUserSettings.json>]\n"
				   "      [--loop-hz <Hz>]\n\n"
				   "  --selftest   校验灵敏度数学 1:1（无需手柄）\n"
				   "  --diag       实机手柄诊断（轴计数/按钮 usage/轮询率）\n"
				   "  --dry-run    只计算不注入（安全验证）\n"
				   "  --always     不做前台闸门，始终注入\n"
				   "  --loop-hz    注入循环频率（默认 16000，与模拟器同级；可 8000 降 CPU）\n"
				   "  Ctrl+C 退出（自动释放所有注入按键）\n");
			return 0;
		}
	}

	if (selftest) return run_selftest(cfg.settings_path);

	load_config_file(cfg, config_path);

	// KovaaK's 灵敏度
	if (cfg.deg_per_count_override > 0.0) {
		cfg.settings_path.clear(); // 显式覆盖，不读文件
	}
	Runtime rt;
	rt.cfg = cfg;
	std::wstring settings = cfg.settings_path;
	if (settings.empty()) settings = autodetect_settings();
	if (!settings.empty()) {
		rt.sens = kovaak_load_sens(settings);
		rt.sens.source_path = utf8_ws(settings);
	}
	if (cfg.deg_per_count_override > 0.0) {
		rt.sens.deg_per_count_x = cfg.deg_per_count_override;
		rt.sens.deg_per_count_y = cfg.deg_per_count_override;
		rt.sens.ok = true;
		rt.sens.note = "config.json deg_per_count_override";
	}

	rt.injector.set_dry_run(dry);

	printf("kovaak_controller — KovaaK's 原生手柄桥（APEX SIMULATOR 同源输入）\n");
	if (dry) printf("*** dry-run：只计算不注入 ***\n");
	print_calibration(rt);

	timeBeginPeriod(1);

	// 读取线程 + 回调
	rt.reader.start(
			[&](uint16_t usage, bool down) { on_button(rt, usage, down); },
			[&](double lx, double ly) { on_left_stick(rt, lx, ly); },
			[&](bool conn, const std::string &path) { on_device(rt, conn, path); });

	if (diag) {
		diag_loop(rt);
	} else {
		// 瞄准主循环线程
		std::thread aim(aim_loop, std::ref(rt));
		// 主线程：周期状态行
		uint64_t last_moves = 0;
		while (!g_shutdown.load()) {
			Sleep(2000);
			if (g_shutdown.load()) break;
			uint64_t m = rt.injector.moves();
			uint64_t dm = m - last_moves;
			last_moves = m;
			double lx = 0, ly = 0, rx = 0, ry = 0;
			if (rt.reader.is_active()) rt.reader.get_axes(lx, ly, rx, ry);
			(void)lx;
			(void)ly;
			printf("[状态] 手柄:%s 轮询:%.0fHz 位深:%d 闸门:%s%s 注入:%.0f cnt/2s R(%+.3f,%+.3f)\n",
					rt.reader.is_active() ? "已连接" : "未连接",
					rt.reader.is_active() ? rt.reader.polling_rate() : 0.0,
					rt.reader.bit_depth(),
					rt.gate_on.load() ? "开" : "关",
					dry ? " [dry-run]" : "",
					double(dm), rx, ry);
		}
		aim.join();
	}

	rt.reader.stop();
	rt.injector.release_all();
	timeEndPeriod(1);
	printf("已退出。\n");
	return 0;
}
