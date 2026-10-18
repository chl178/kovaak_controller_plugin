// injector.h — SendInput 注入器：把旋转角/按键事件注入前台（KovaaK's）。
//
// 鼠标：MOUSEEVENTF_MOVE 相对计数 —— 与真实鼠标完全相同的通道，
// KovaaK's（UE4 Raw Input）按计数×灵敏度旋转视角，无光标路径语义。
// 键盘/鼠标键：边缘事件（按下/抬起）。
//
// held 状态始终跟踪（即使闸门关闭）；闸门重新打开时 sync() 重发所有
// 仍按下的键（例：切回 KovaaK's 窗口时摇杆仍偏转 → W 重新按下）。
// 闸门关闭/手柄断连/退出 → release_all()。
#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <set>

class Injector {
public:
	void set_enabled(bool on) {
		if (enabled_.exchange(on) == on) return;
		if (!on) {
			release_all();
		} else {
			sync();
		}
	}
	bool enabled() const { return enabled_.load(); }
	void set_dry_run(bool on) {
		if (dry_.exchange(on) && !on) {
			// dry-run 关闭：状态可能已积累，重新同步
			sync();
		}
	}
	bool dry_run() const { return dry_.load(); }

	// 相对鼠标移动（整数计数；小数由 AimLoop 的累加器截断产生）
	void mouse_move(int dx, int dy) {
		if (dx == 0 && dy == 0) return;
		if (!enabled_.load() || dry_.load()) {
			stats_moves_++;
			stats_dx_ += dx > 0 ? dx : -dx;
			stats_dy_ += dy > 0 ? dy : -dy;
			return;
		}
		INPUT in{};
		in.type = INPUT_MOUSE;
		in.mi.dwFlags = MOUSEEVENTF_MOVE;
		in.mi.dx = dx;
		in.mi.dy = dy;
		if (SendInput(1, &in, sizeof(INPUT)) == 1) {
			stats_moves_++;
			stats_dx_ += dx > 0 ? dx : -dx;
			stats_dy_ += dy > 0 ? dy : -dy;
		}
	}

	void mouse_button(bool left, bool down) {
		std::lock_guard<std::mutex> lock(mtx_);
		bool &held = left ? lmb_ : rmb_;
		if (held == down) return;
		held = down;
		if (!enabled_.load() || dry_.load()) return;
		INPUT in{};
		in.type = INPUT_MOUSE;
		in.mi.dwFlags = left ? (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP)
								: (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
		SendInput(1, &in, sizeof(INPUT));
	}

	void key(WORD vk, bool down) {
		std::lock_guard<std::mutex> lock(mtx_);
		bool held = held_keys_.count(vk) != 0;
		if (held == down) return;
		if (down) held_keys_.insert(vk);
		else held_keys_.erase(vk);
		if (!enabled_.load() || dry_.load()) return;
		INPUT in{};
		in.type = INPUT_KEYBOARD;
		in.ki.wVk = vk;
		in.ki.wScan = 0;
		in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
		SendInput(1, &in, sizeof(INPUT));
	}

	// 释放所有按下的键/按钮（闸门关闭、手柄断连、退出时调用）
	void release_all() {
		std::lock_guard<std::mutex> lock(mtx_);
		if (enabled_.load() && !dry_.load()) {
			if (lmb_) {
				INPUT in{};
				in.type = INPUT_MOUSE;
				in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
				SendInput(1, &in, sizeof(INPUT));
			}
			if (rmb_) {
				INPUT in{};
				in.type = INPUT_MOUSE;
				in.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
				SendInput(1, &in, sizeof(INPUT));
			}
			for (WORD vk : held_keys_) {
				INPUT in{};
				in.type = INPUT_KEYBOARD;
				in.ki.wVk = vk;
				in.ki.dwFlags = KEYEVENTF_KEYUP;
				SendInput(1, &in, sizeof(INPUT));
			}
		}
		lmb_ = false;
		rmb_ = false;
		held_keys_.clear();
	}

	// 统计（诊断输出用）
	uint64_t moves() const { return stats_moves_.load(); }
	uint64_t abs_dx() const { return stats_dx_.load(); }
	uint64_t abs_dy() const { return stats_dy_.load(); }

private:
	// 闸门开启时把 held 状态真实下发
	void sync() {
		std::lock_guard<std::mutex> lock(mtx_);
		if (!enabled_.load() || dry_.load()) return;
		if (lmb_) {
			INPUT in{};
			in.type = INPUT_MOUSE;
			in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
			SendInput(1, &in, sizeof(INPUT));
		}
		if (rmb_) {
			INPUT in{};
			in.type = INPUT_MOUSE;
			in.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
			SendInput(1, &in, sizeof(INPUT));
		}
		for (WORD vk : held_keys_) {
			INPUT in{};
			in.type = INPUT_KEYBOARD;
			in.ki.wVk = vk;
			in.ki.dwFlags = 0;
			SendInput(1, &in, sizeof(INPUT));
		}
	}

	std::atomic<bool> enabled_{false};
	std::atomic<bool> dry_{false};
	std::mutex mtx_;
	bool lmb_ = false;
	bool rmb_ = false;
	std::set<WORD> held_keys_;
	std::atomic<uint64_t> stats_moves_{0};
	std::atomic<uint64_t> stats_dx_{0};
	std::atomic<uint64_t> stats_dy_{0};
};
