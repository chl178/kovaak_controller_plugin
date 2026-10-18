// json_min.h — 最小 JSON 解析器（无第三方依赖）。
// 只用于读取 KovaaK's 的 PrimaryUserSettings.json / FovSensConfig.json
// 与本工具的 config.json。支持对象/数组/字符串/数字/布尔/null 与 \u 转义。
#pragma once

#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace jmin {

struct Value;
using Ptr = std::shared_ptr<Value>;

struct Value {
	enum Type { NUL, BOOL, NUM, STR, ARR, OBJ };
	Type type = NUL;
	bool b = false;
	double num = 0.0;
	std::string str;
	std::vector<Ptr> arr;
	std::unordered_map<std::string, Ptr> obj;

	bool has(const std::string &k) const { return type == OBJ && obj.count(k) != 0; }
	Ptr get(const std::string &k) const {
		if (type != OBJ) return nullptr;
		auto it = obj.find(k);
		return it == obj.end() ? nullptr : it->second;
	}
	double num_or(double dflt) const { return type == NUM ? num : dflt; }
	bool bool_or(bool dflt) const { return type == BOOL ? b : dflt; }
	const std::string &str_or(const std::string &dflt) const { return type == STR ? str : dflt; }
};

inline void utf8_append(std::string &out, unsigned cp) {
	if (cp < 0x80) {
		out.push_back(char(cp));
	} else if (cp < 0x800) {
		out.push_back(char(0xC0 | (cp >> 6)));
		out.push_back(char(0x80 | (cp & 0x3F)));
	} else if (cp < 0x10000) {
		out.push_back(char(0xE0 | (cp >> 12)));
		out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(char(0x80 | (cp & 0x3F)));
	} else {
		out.push_back(char(0xF0 | (cp >> 18)));
		out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
		out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
		out.push_back(char(0x80 | (cp & 0x3F)));
	}
}

class Parser {
public:
	explicit Parser(const std::string &text) : s_(text) {}

	bool parse(Ptr &out, std::string &err) {
		try {
			skip_ws();
			out = parse_value(0);
			skip_ws();
			if (pos_ != s_.size()) fail("trailing characters");
			return true;
		} catch (const std::string &e) {
			err = e + " @ offset " + std::to_string(pos_);
			return false;
		}
	}

private:
	const std::string &s_;
	size_t pos_ = 0;
	static constexpr int kMaxDepth = 32;

	[[noreturn]] void fail(const char *msg) { throw std::string(msg); }
	void skip_ws() {
		while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n'))
			++pos_;
	}
	char peek() {
		if (pos_ >= s_.size()) fail("unexpected end");
		return s_[pos_];
	}
	void expect(char c) {
		if (pos_ >= s_.size() || s_[pos_] != c) fail("unexpected char");
		++pos_;
	}
	bool literal(const char *lit) {
		size_t n = strlen_(lit);
		if (s_.compare(pos_, n, lit) == 0) {
			pos_ += n;
			return true;
		}
		return false;
	}
	static size_t strlen_(const char *p) { return std::char_traits<char>::length(p); }

	Ptr parse_value(int depth) {
		if (depth > kMaxDepth) fail("too deep");
		skip_ws();
		char c = peek();
		if (c == '{') return parse_obj(depth);
		if (c == '[') return parse_arr(depth);
		if (c == '"') {
			auto v = std::make_shared<Value>();
			v->type = Value::STR;
			v->str = parse_string();
			return v;
		}
		if (literal("true")) {
			auto v = std::make_shared<Value>();
			v->type = Value::BOOL;
			v->b = true;
			return v;
		}
		if (literal("false")) {
			auto v = std::make_shared<Value>();
			v->type = Value::BOOL;
			v->b = false;
			return v;
		}
		if (literal("null")) return std::make_shared<Value>();
		return parse_num();
	}

	Ptr parse_obj(int depth) {
		auto v = std::make_shared<Value>();
		v->type = Value::OBJ;
		expect('{');
		skip_ws();
		if (peek() == '}') {
			++pos_;
			return v;
		}
		for (;;) {
			skip_ws();
			std::string key = parse_string();
			skip_ws();
			expect(':');
			v->obj[key] = parse_value(depth + 1);
			skip_ws();
			char c = peek();
			if (c == ',') {
				++pos_;
				continue;
			}
			if (c == '}') {
				++pos_;
				return v;
			}
			fail("expected , or }");
		}
	}

	Ptr parse_arr(int depth) {
		auto v = std::make_shared<Value>();
		v->type = Value::ARR;
		expect('[');
		skip_ws();
		if (peek() == ']') {
			++pos_;
			return v;
		}
		for (;;) {
			v->arr.push_back(parse_value(depth + 1));
			skip_ws();
			char c = peek();
			if (c == ',') {
				++pos_;
				continue;
			}
			if (c == ']') {
				++pos_;
				return v;
			}
			fail("expected , or ]");
		}
	}

	std::string parse_string() {
		expect('"');
		std::string out;
		for (;;) {
			if (pos_ >= s_.size()) fail("unterminated string");
			char c = s_[pos_++];
			if (c == '"') return out;
			if (c != '\\') {
				out.push_back(c);
				continue;
			}
			if (pos_ >= s_.size()) fail("bad escape");
			char e = s_[pos_++];
			switch (e) {
			case '"': out.push_back('"'); break;
			case '\\': out.push_back('\\'); break;
			case '/': out.push_back('/'); break;
			case 'b': out.push_back('\b'); break;
			case 'f': out.push_back('\f'); break;
			case 'n': out.push_back('\n'); break;
			case 'r': out.push_back('\r'); break;
			case 't': out.push_back('\t'); break;
			case 'u': {
				unsigned cp = hex4();
				if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
					size_t save = pos_;
					pos_ += 2;
					unsigned lo = hex4();
					if (lo >= 0xDC00 && lo <= 0xDFFF) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					} else {
						pos_ = save;
					}
				}
				utf8_append(out, cp);
				break;
			}
			default: fail("bad escape char");
			}
		}
	}

	unsigned hex4() {
		if (pos_ + 4 > s_.size()) fail("bad \\u");
		unsigned v = 0;
		for (int i = 0; i < 4; ++i) {
			char c = s_[pos_++];
			v <<= 4;
			if (c >= '0' && c <= '9') v |= unsigned(c - '0');
			else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
			else fail("bad hex");
		}
		return v;
	}

	Ptr parse_num() {
		size_t start = pos_;
		if (pos_ < s_.size() && (s_[pos_] == '-' || s_[pos_] == '+')) ++pos_;
		bool any = false;
		while (pos_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E' || s_[pos_] == '+' || s_[pos_] == '-')) {
			// '+'/'-' 仅允许跟在 e/E 后
			if ((s_[pos_] == '+' || s_[pos_] == '-') && pos_ > start && s_[pos_ - 1] != 'e' && s_[pos_ - 1] != 'E') break;
			++pos_;
			any = true;
		}
		if (!any) fail("invalid number");
		auto v = std::make_shared<Value>();
		v->type = Value::NUM;
		v->num = std::strtod(s_.substr(start, pos_ - start).c_str(), nullptr);
		return v;
	}
};

inline bool parse(const std::string &text, Ptr &out, std::string &err) {
	if (!text.empty() && static_cast<unsigned char>(text[0]) == 0xEF && text.size() > 2 && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
		Parser p(text.substr(3));
		return p.parse(out, err);
	}
	Parser p(text);
	return p.parse(out, err);
}

} // namespace jmin
