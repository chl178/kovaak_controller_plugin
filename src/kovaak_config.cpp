// kovaak_config.cpp — 见头文件说明。
#include "kovaak_config.h"

#include "expr_eval.h"
#include "json_min.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace {

std::string read_text_file(const std::wstring &path) {
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return "";
	DWORD size = GetFileSize(h, nullptr);
	if (size == 0 || size > 8u * 1024 * 1024) {
		CloseHandle(h);
		return "";
	}
	std::string out(size, '\0');
	DWORD read_n = 0;
	BOOL ok = ReadFile(h, out.data(), size, &read_n, nullptr);
	CloseHandle(h);
	if (!ok) return "";
	out.resize(read_n);
	return out;
}

// 宽字符串路径 → 窄字符（仅用于显示，按 UTF-8 转换）
std::string wide_to_utf8(const std::wstring &w) {
	if (w.empty()) return "";
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
	std::string out(size_t(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), out.data(), n, nullptr, nullptr);
	return out;
}

// 在 settings 顶层分组（booleanSettings/integerSettings/floatSettings/stringSettings）
// 中按键名后缀（::后的部分）查找。
jmin::Ptr find_by_suffix(const jmin::Ptr &root, const char *group, const std::string &suffix) {
	jmin::Ptr g = root->get(group);
	if (!g || g->type != jmin::Value::OBJ) return nullptr;
	for (auto &kv : g->obj) {
		if (kv.first.size() >= suffix.size() &&
				kv.first.compare(kv.first.size() - suffix.size(), suffix.size(), suffix) == 0)
			return kv.second;
	}
	return nullptr;
}

} // namespace

KovaakSens kovaak_load_sens(const std::wstring &settings_path) {
	KovaakSens out;
	out.source_path = wide_to_utf8(settings_path);

	std::string text = read_text_file(settings_path);
	if (text.empty()) {
		out.note = "PrimaryUserSettings.json unreadable -> fallback 0.022 deg/count";
		return out;
	}
	jmin::Ptr root;
	std::string err;
	if (!jmin::parse(text, root, err)) {
		out.note = "settings JSON parse failed (" + err + ") -> fallback 0.022";
		return out;
	}

	jmin::Ptr jx = find_by_suffix(root, "floatSettings", "XSens");
	jmin::Ptr jy = find_by_suffix(root, "floatSettings", "YSens");
	jmin::Ptr jscale = find_by_suffix(root, "stringSettings", "SensScaleString");
	jmin::Ptr jdpi = find_by_suffix(root, "integerSettings", "DPI");
	jmin::Ptr jfov = find_by_suffix(root, "floatSettings", "FOV");
	jmin::Ptr jinv = find_by_suffix(root, "booleanSettings", "YInvert");

	out.xsens = jx ? jx->num_or(1.0) : 1.0;
	out.ysens = jy ? jy->num_or(out.xsens) : out.xsens;
	out.dpi = jdpi ? int(jdpi->num_or(800)) : 800;
	out.fov = jfov ? jfov->num_or(100.0) : 100.0;
	out.y_invert = jinv ? jinv->bool_or(false) : false;
	out.scale_name = jscale ? jscale->str_or("") : "";

	// FovSensConfig.json 与设置文件同目录
	size_t slash = settings_path.find_last_of(L"\\/");
	std::wstring dir = slash == std::wstring::npos ? L"." : settings_path.substr(0, slash);
	std::wstring fovsens_path = dir + L"\\FovSensConfig.json";
	std::string ftext = read_text_file(fovsens_path);
	jmin::Ptr froot;
	if (ftext.empty() || !jmin::parse(ftext, froot, err) || !froot || froot->type != jmin::Value::ARR) {
		out.note = "FovSensConfig.json missing/unparsable -> fallback 0.022";
		return out;
	}

	// 找刻度名对应的 IncrementFormula
	std::string formula;
	for (auto &entry : froot->arr) {
		jmin::Ptr name = entry->get("ScaleName");
		if (name && name->type == jmin::Value::STR && name->str == out.scale_name) {
			jmin::Ptr sens = entry->get("Sens");
			jmin::Ptr inc = sens ? sens->get("IncrementFormula") : nullptr;
			if (inc && inc->type == jmin::Value::STR) formula = inc->str;
			break;
		}
	}

	if (formula.empty()) {
		out.note = "scale '" + out.scale_name + "' has no IncrementFormula -> fallback 0.022";
		return out;
	}

	auto resolve_x = [&](const std::string &n) -> double {
		if (n == "Sens") return out.xsens;
		if (n == "DPI") return double(out.dpi);
		if (n == "FOV") return out.fov;
		if (n == "_pi") return 3.14159265358979323846;
		return std::nan("");
	};
	auto resolve_y = [&](const std::string &n) -> double {
		if (n == "Sens") return out.ysens;
		if (n == "DPI") return double(out.dpi);
		if (n == "FOV") return out.fov;
		if (n == "_pi") return 3.14159265358979323846;
		return std::nan("");
	};

	double dx = ExprEval::eval(formula, resolve_x);
	double dy = ExprEval::eval(formula, resolve_y);
	if (std::isnan(dx) || dx <= 0.0 || std::isnan(dy) || dy <= 0.0) {
		out.note = "formula eval failed: '" + formula + "' -> fallback 0.022";
		return out;
	}

	out.deg_per_count_x = dx;
	out.deg_per_count_y = dy;
	out.ok = true;
	char buf[256];
	snprintf(buf, sizeof(buf), "scale '%s': %.4g x %.4g = %.5g deg/count",
			out.scale_name.c_str(), out.xsens, dx / (out.xsens > 0 ? out.xsens : 1.0), dx);
	out.note = buf;
	return out;
}
