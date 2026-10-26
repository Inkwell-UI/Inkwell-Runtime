#include "Ini.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>

namespace inkwell
{
	namespace
	{
		std::string Lower(std::string_view s)
		{
			std::string r(s);
			std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return r;
		}

		std::string Trim(std::string_view s)
		{
			const auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r' || c == '\n'; };
			while (!s.empty() && ws(s.front())) s.remove_prefix(1);
			while (!s.empty() && ws(s.back())) s.remove_suffix(1);
			return std::string(s);
		}

		bool IsDigit(char c) { return c >= '0' && c <= '9'; }

		// /^[+-]?\d+$/
		bool IsWholeNumber(const std::string& s)
		{
			std::size_t i = (!s.empty() && (s[0] == '+' || s[0] == '-')) ? 1 : 0;
			if (i >= s.size()) return false;
			return std::all_of(s.begin() + static_cast<std::ptrdiff_t>(i), s.end(), IsDigit);
		}

		// /^[+-]?(\d+\.?\d*|\.\d+)$/
		bool IsDecimal(const std::string& s)
		{
			std::size_t i = (!s.empty() && (s[0] == '+' || s[0] == '-')) ? 1 : 0;
			std::size_t digitsBefore = 0, digitsAfter = 0;
			while (i < s.size() && IsDigit(s[i])) { ++i; ++digitsBefore; }
			if (i < s.size() && s[i] == '.') {
				++i;
				while (i < s.size() && IsDigit(s[i])) { ++i; ++digitsAfter; }
				if (digitsBefore == 0 && digitsAfter == 0) return false;
			} else if (digitsBefore == 0) {
				return false;
			}
			return i == s.size();
		}

		// A setter returns a problem message, or an empty string when the value was applied cleanly.
		struct Target
		{
			MenuIni*                   menu{ nullptr };
			WidgetIni*                 widget{ nullptr };
			ParsedIni*                 ini{ nullptr };  // for Runtime/Dev
		};
		using Setter = std::function<std::string(Target&, const std::string&)>;

		std::string Num(double d)
		{
			// JS prints integers without ".0"
			if (d == static_cast<long long>(d)) return std::to_string(static_cast<long long>(d));
			return std::to_string(d);
		}

		Setter IntSetter(double min, double max, std::function<void(Target&, int)> set)
		{
			return [=](Target& t, const std::string& raw) -> std::string {
				if (!IsWholeNumber(raw)) return "expected a whole number, got '" + raw + "'";
				const double n = std::strtod(raw.c_str(), nullptr);
				if (n < min || n > max) return Num(n) + " is outside " + Num(min) + ".." + Num(max);
				set(t, static_cast<int>(n));
				return {};
			};
		}

		Setter FlagSetter(std::function<void(Target&, bool)> set)
		{
			return [=](Target& t, const std::string& raw) -> std::string {
				if (raw == "0" || raw == "1") {
					set(t, raw == "1");
					return {};
				}
				return "expected 0 or 1, got '" + raw + "'";
			};
		}

		using Table = std::map<std::string, Setter>;

		const Table& MenuKeys()
		{
			static const Table t = {
				{ "openkey", [](Target& t, const std::string& raw) -> std::string {
					 if (IsWholeNumber(raw)) {
						 const double n = std::strtod(raw.c_str(), nullptr);
						 if (n == 0 || (n >= 2 && n <= 255)) {
							 t.menu->openKey = static_cast<int>(n);
							 return {};
						 }
					 }
					 return "expected 0 or 2..255, got '" + raw + "'";
				 } },
				{ "uiscale", [](Target& t, const std::string& raw) -> std::string {
					 if (!IsDecimal(raw)) return "expected a number, got '" + raw + "'";
					 const double n = std::strtod(raw.c_str(), nullptr);
					 const double c = std::min(kUiScaleMax, std::max(kUiScaleMin, n));
					 t.menu->uiScale = c;
					 if (c != n) return "clamped " + std::to_string(n) + " to " + std::to_string(c);  // the value is kept, clamped
					 return {};
				 } },
				{ "offsetx", IntSetter(-960, 960, [](Target& t, int n) { t.menu->offsetX = n; }) },
				{ "offsety", IntSetter(-540, 540, [](Target& t, int n) { t.menu->offsetY = n; }) },
				{ "theme", [](Target& t, const std::string& raw) -> std::string {
					 if (raw.empty()) t.menu->theme.reset();
					 else t.menu->theme = raw;
					 return {};
				 } },
			};
			return t;
		}

		const Table& WidgetKeys()
		{
			static const Table t = {
				{ "offsetx", IntSetter(-100000, 100000, [](Target& t, int n) { t.widget->offsetX = n; }) },
				{ "offsety", IntSetter(-100000, 100000, [](Target& t, int n) { t.widget->offsetY = n; }) },
				{ "scale", [](Target& t, const std::string& raw) -> std::string {
					 if (IsDecimal(raw) && std::strtod(raw.c_str(), nullptr) > 0) {
						 t.widget->scale = std::strtod(raw.c_str(), nullptr);
						 return {};
					 }
					 return "expected a positive number, got '" + raw + "'";
				 } },
			};
			return t;
		}

		const Table& RuntimeKeys()
		{
			static const Table t = {
				{ "loglevel", [](Target& t, const std::string& raw) -> std::string {
					 const std::string l = Lower(raw);
					 if (l == "trace" || l == "debug" || l == "info" || l == "warn" || l == "error") {
						 t.ini->logLevel = l;
						 return {};
					 }
					 return "expected one of trace, debug, info, warn, error, got '" + raw + "'";
				 } },
			};
			return t;
		}

		bool IsToken(const std::string& s)
		{
			return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) { return IsDigit(c) || (c >= 'a' && c <= 'f'); });
		}

		const Table& DevKeys()
		{
			static const Table t = {
				{ "devmode", FlagSetter([](Target& t, bool b) { t.ini->devMode = b; }) },
				{ "port", IntSetter(1024, 65535, [](Target& t, int n) { t.ini->port = n; }) },
				{ "token", [](Target& t, const std::string& raw) -> std::string {
					 if (raw.empty()) {
						 t.ini->token.reset();
						 return {};
					 }
					 if (IsToken(raw)) {
						 t.ini->token = raw;
						 return {};
					 }
					 return "expected 64 lower-case hex digits";
				 } },
				{ "watch", FlagSetter([](Target& t, bool b) { t.ini->watch = b; }) },
			};
			return t;
		}

		template <class T>
		T& GetOrAdd(std::vector<std::pair<std::string, T>>& bag, const std::string& name)
		{
			for (auto& [n, v] : bag) {
				if (n == name) return v;
			}
			bag.emplace_back(name, T{});
			return bag.back().second;
		}
	}

	const MenuIni* ParsedIni::FindMenu(std::string_view name) const
	{
		const std::string k = Lower(name);
		for (const auto& [n, m] : menus) {
			if (Lower(n) == k) return &m;
		}
		return nullptr;
	}

	ParsedIni ParseIni(std::string_view text, IniKind kind)
	{
		ParsedIni out;
		if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) text.remove_prefix(3);

		std::map<std::string, std::string> menuNames, widgetNames;  // lower -> first spelling
		std::string  section;
		Target       target;
		const Table* keys = nullptr;
		bool         sawRuntime = false, sawDev = false;

		// split on \r\n | \r | \n
		std::vector<std::string_view> lines;
		{
			std::size_t start = 0;
			for (std::size_t i = 0; i <= text.size(); ++i) {
				if (i == text.size() || text[i] == '\n' || text[i] == '\r') {
					lines.push_back(text.substr(start, i - start));
					if (i < text.size() && text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
					start = i + 1;
				}
			}
		}

		int no = 0;
		for (const auto rawLine : lines) {
			++no;
			const std::string line = Trim(rawLine);
			if (line.empty() || line[0] == ';' || line[0] == '#') continue;

			if (line.front() == '[' && line.back() == ']') {
				section = Trim(std::string_view(line).substr(1, line.size() - 2));
				target = Target{};
				keys = nullptr;
				const std::string lower = Lower(section);
				const auto        sub = [&](std::size_t prefixLen) { return Trim(std::string_view(section).substr(prefixLen)); };
				if (lower.rfind("menu.", 0) == 0 && !sub(5).empty()) {
					const std::string s = sub(5);
					const auto        it = menuNames.emplace(Lower(s), s).first;
					target.menu = &GetOrAdd(out.menus, it->second);
					keys = &MenuKeys();
				} else if (lower.rfind("widget.", 0) == 0 && !sub(7).empty()) {
					const std::string s = sub(7);
					const auto        it = widgetNames.emplace(Lower(s), s).first;
					target.widget = &GetOrAdd(out.widgets, it->second);
					keys = &WidgetKeys();
				}
				if (lower == "runtime" || lower == "dev") {
					if (kind == IniKind::kMod) {
						out.problems.push_back({ no, section, "", "[" + section + "] is only read from Inkwell.ini; ignored" });
					} else {
						target.ini = &out;
						keys = lower == "dev" ? &DevKeys() : &RuntimeKeys();
						(lower == "dev" ? sawDev : sawRuntime) = true;
					}
				}
				continue;
			}

			const auto eq = line.find('=');
			if (eq == std::string::npos) {
				out.problems.push_back({ no, section, "", "not a key=value line; ignored" });
				continue;
			}
			const std::string key = Trim(std::string_view(line).substr(0, eq));
			const std::string value = Trim(std::string_view(line).substr(eq + 1));
			out.raw[Lower(section)][Lower(key)] = value;  // raw copy for getSetting; last duplicate wins
			if (!keys) continue;                           // unknown section, or a section skipped above
			const auto set = keys->find(Lower(key));
			if (set == keys->end()) continue;              // unknown key
			const std::string msg = set->second(target, value);
			if (!msg.empty()) out.problems.push_back({ no, section, key, msg });
		}

		// [Runtime]/[Dev] count as present only with at least one applied key (the Spec deletes empty ones)
		out.hasRuntimeSection = sawRuntime && out.logLevel.has_value();
		out.hasDevSection = sawDev && (out.devMode || out.port || out.token || out.watch);
		return out;
	}

	EffectiveMenu ResolveMenu(std::optional<int> jsonHotkey, const MenuIni* ini)
	{
		EffectiveMenu e;
		const std::optional<int> key = (ini && ini->openKey) ? ini->openKey : jsonHotkey;
		if (key && *key != 0) e.openKey = key;
		if (ini) {
			if (ini->uiScale) e.uiScale = *ini->uiScale;
			if (ini->offsetX) e.offsetX = *ini->offsetX;
			if (ini->offsetY) e.offsetY = *ini->offsetY;
			if (ini->theme) e.theme = *ini->theme;
		}
		return e;
	}
}
