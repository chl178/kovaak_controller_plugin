// expr_eval.h — 极小数学表达式求值器。
// 用于计算 KovaaK's FovSensConfig.json 中的 IncrementFormula 字符串，
// 例如 "Sens * 0.022"、"Sens * 0.104156 * FOV * 0.001"、
// "0.00005555 * 10 ^ (Sens / 50) * FOV"、"(Sens * 0.005446) + 0.032809"。
//
// 变量通过 resolver 提供（Sens / DPI / FOV / _pi 等）。
// 支持 + - * / ^（右结合幂）、一元正负、括号。出错返回 NaN。
#pragma once

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <string>

class ExprEval {
public:
	using Resolver = std::function<double(const std::string &)>;

	static double eval(const std::string &expr, const Resolver &resolve) {
		try {
			ExprEval e(expr, resolve);
			double v = e.parse_expr();
			e.skip_ws();
			if (e.pos_ != e.s_.size()) return std::nan("");
			return v;
		} catch (...) {
			return std::nan("");
		}
	}

private:
	ExprEval(const std::string &s, Resolver r) : s_(s), resolve_(std::move(r)) {}

	const std::string &s_;
	Resolver resolve_;
	size_t pos_ = 0;

	void skip_ws() {
		while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
	}
	bool eat(char c) {
		skip_ws();
		if (pos_ < s_.size() && s_[pos_] == c) {
			++pos_;
			return true;
		}
		return false;
	}
	[[noreturn]] void fail() { throw 0; }

	double parse_expr() {
		double v = parse_term();
		for (;;) {
			if (eat('+')) v += parse_term();
			else if (eat('-')) v -= parse_term();
			else return v;
		}
	}
	double parse_term() {
		double v = parse_unary();
		for (;;) {
			if (eat('*')) v *= parse_unary();
			else if (eat('/')) {
				double d = parse_unary();
				if (d == 0.0) fail();
				v /= d;
			} else return v;
		}
	}
	double parse_unary() {
		if (eat('-')) return -parse_unary();
		if (eat('+')) return parse_unary();
		return parse_power();
	}
	double parse_power() {
		double base = parse_primary();
		if (eat('^')) return std::pow(base, parse_unary()); // 右结合
		return base;
	}
	double parse_primary() {
		skip_ws();
		if (pos_ >= s_.size()) fail();
		char c = s_[pos_];
		if (c == '(') {
			++pos_;
			double v = parse_expr();
			if (!eat(')')) fail();
			return v;
		}
		if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') return parse_number();
		if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') return parse_ident();
		fail();
	}
	double parse_number() {
		size_t start = pos_;
		while (pos_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '.')) ++pos_;
		// 指数（如 1e-3）
		if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
			++pos_;
			if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
			while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) ++pos_;
		}
		if (pos_ == start) fail();
		return std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
	}
	double parse_ident() {
		size_t start = pos_;
		while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_')) ++pos_;
		std::string name = s_.substr(start, pos_ - start);
		double v = resolve_(name);
		if (std::isnan(v)) fail();
		return v;
	}
};
