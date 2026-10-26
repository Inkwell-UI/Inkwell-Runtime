#include "MenuConfig.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>

namespace inkwell
{
	using json = nlohmann::json;

	namespace
	{
		bool IsAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
		bool IsDigit(char c) { return c >= '0' && c <= '9'; }

		std::string Lower(std::string_view s)
		{
			std::string r(s);
			std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return r;
		}

		bool StartsWithNoCase(std::string_view s, std::string_view prefix)
		{
			return s.size() >= prefix.size() && Lower(s.substr(0, prefix.size())) == prefix;
		}

		// zod `z.number().int()`: a JSON number with no fractional part (6.0 passes, like in JS).
		bool AsInt(const json& j, long long& out)
		{
			if (j.is_number_integer()) {
				out = j.get<long long>();
				return true;
			}
			if (j.is_number_float()) {
				const double d = j.get<double>();
				if (std::isfinite(d) && std::floor(d) == d && std::fabs(d) < 9e15) {
					out = static_cast<long long>(d);
					return true;
				}
			}
			return false;
		}

		struct Failure
		{
			std::string text;
		};

		// Throws Failure with "<path>: <reason>"; ParseMenuDef catches it and turns it into the single error line.
		[[noreturn]] void Fail(const std::string& path, const std::string& why) { throw Failure{ path + ": " + why }; }

		bool GetBool(const json& obj, const char* key, const std::string& path, bool dflt)
		{
			const auto it = obj.find(key);
			if (it == obj.end()) return dflt;
			if (!it->is_boolean()) Fail(path + "." + key, "expected true or false");
			return it->get<bool>();
		}

		void NoteUnknown(const json& obj, std::initializer_list<const char*> known, const std::string& where, std::vector<Problem>& out)
		{
			for (auto it = obj.begin(); it != obj.end(); ++it) {
				if (std::none_of(known.begin(), known.end(), [&](const char* k) { return it.key() == k; })) {
					out.push_back({ Problem::Level::kDebug, "unknown key '" + (where.empty() ? "" : where + ".") + it.key() + "' ignored" });
				}
			}
		}

		// ^\d{1,3}\.\d{1,3}\.\d{1,3}$
		bool IsMinRuntimeShape(const std::string& s)
		{
			std::size_t pos = 0;
			for (int part = 0; part < 3; ++part) {
				std::size_t digits = 0;
				while (pos < s.size() && IsDigit(s[pos])) {
					++pos;
					++digits;
				}
				if (digits < 1 || digits > 3) return false;
				if (part < 2) {
					if (pos >= s.size() || s[pos] != '.') return false;
					++pos;
				}
			}
			return pos == s.size();
		}
	}

	std::string MenuNameKey(std::string_view name) { return Lower(name); }

	bool IsValidMenuName(std::string_view n)
	{
		if (n.size() < 2 || n.size() > 64 || !IsAlpha(n[0])) return false;
		return std::all_of(n.begin() + 1, n.end(), [](char c) { return IsAlpha(c) || IsDigit(c) || c == '_'; });
	}

	bool IsValidModEventName(std::string_view n)
	{
		if (n.empty() || n.size() > 64) return false;
		return std::all_of(n.begin(), n.end(), [](char c) { return IsAlpha(c) || IsDigit(c) || c == '_'; });
	}

	std::optional<std::string> NormalizeSwfPath(std::string_view swf)
	{
		if (swf.find('\\') != std::string_view::npos || swf.find(':') != std::string_view::npos || (!swf.empty() && swf[0] == '/')) return std::nullopt;
		if (swf.size() < 14 || !StartsWithNoCase(swf, "interface/") || Lower(swf.substr(swf.size() - 4)) != ".swf") return std::nullopt;
		const std::string_view bare = swf.substr(10, swf.size() - 14);
		if (bare.empty()) return std::nullopt;
		std::size_t start = 0;
		while (true) {
			const auto             end = bare.find('/', start);
			const std::string_view seg = bare.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
			if (seg.empty() || seg == "." || seg == "..") return std::nullopt;
			if (end == std::string_view::npos) break;
			start = end + 1;
		}
		return std::string(bare);
	}

	MenuParseResult ParseMenuDef(std::string_view text, const Version& have)
	{
		MenuParseResult res;
		auto            error = [&](const std::string& msg) { res.problems.push_back({ Problem::Level::kError, msg }); };
		try {
			// strip a UTF-8 BOM
			if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) text.remove_prefix(3);
			const json j = json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions*/ false);
			if (j.is_discarded()) {
				error("not valid JSON");
				return res;
			}
			if (!j.is_object()) {
				error("expected a JSON object");
				return res;
			}

			MenuDef d;

			// specVersion first, so an old file (the spike form) reports "unsupported spec version" instead of a field error
			{
				const auto it = j.find("specVersion");
				long long  v = 0;
				if (it == j.end()) Fail("specVersion", "required");
				if (!AsInt(*it, v)) Fail("specVersion", "expected a whole number");
				if (v < kMenuMinSpecVersion) Fail("specVersion", "unsupported spec version " + std::to_string(v) + " (this runtime needs " + std::to_string(kMenuMinSpecVersion) + " or newer)");
				d.specVersion = static_cast<int>(v);
				if (v > kSpecVersion) res.problems.push_back({ Problem::Level::kWarn, "specVersion " + std::to_string(v) + " is newer than this runtime's " + std::to_string(kSpecVersion) + "; parsed anyway" });
			}

			{
				const auto it = j.find("name");
				if (it == j.end()) Fail("name", "required");
				if (!it->is_string() || !IsValidMenuName(it->get_ref<const std::string&>())) Fail("name", "must match ^[A-Za-z][A-Za-z0-9_]{1,63}$");
				d.name = it->get<std::string>();
			}
			{
				const auto it = j.find("swf");
				if (it == j.end()) Fail("swf", "required");
				if (!it->is_string()) Fail("swf", "expected a string");
				d.swf = it->get<std::string>();
				const auto bare = NormalizeSwfPath(d.swf);
				if (!bare) Fail("swf", "must be relative to Data/, start with Interface/, end with .swf, use / and contain no '..', ':' or leading '/'");
				d.bareSwf = *bare;
			}
			if (const auto it = j.find("minRuntime"); it != j.end()) {
				if (!it->is_string() || !IsMinRuntimeShape(it->get_ref<const std::string&>())) Fail("minRuntime", "must look like 0.3.0 (MAJOR.MINOR.PATCH, 1 to 3 digits each)");
				d.minRuntime = it->get<std::string>();
			}

			if (const auto it = j.find("flags"); it != j.end()) {
				if (!it->is_object()) Fail("flags", "expected an object");
				d.pausesGame = GetBool(*it, "pausesGame", "flags", false);
				d.cursor = GetBool(*it, "cursor", "flags", false);
				d.modal = GetBool(*it, "modal", "flags", false);
				d.closeOnEscape = GetBool(*it, "closeOnEscape", "flags", true);
				NoteUnknown(*it, { "pausesGame", "cursor", "modal", "closeOnEscape" }, "flags", res.problems);
			}

			if (const auto it = j.find("inputContext"); it != j.end()) {
				if (!it->is_string()) Fail("inputContext", "must be \"none\" or \"menuMode\"");
				const auto& s = it->get_ref<const std::string&>();
				if (s == "none") d.inputContext = InputContext::kNone;
				else if (s == "menuMode") d.inputContext = InputContext::kMenuMode;
				else Fail("inputContext", "must be \"none\" or \"menuMode\" (other contexts arrive in a later version), got \"" + s + "\"");
				d.inputContextExplicit = true;
			} else {
				d.inputContext = d.modal ? InputContext::kMenuMode : InputContext::kNone;
			}

			if (const auto it = j.find("depth"); it != j.end()) {
				long long v = 0;
				if (!AsInt(*it, v) || v < 0 || v > 15) Fail("depth", "expected a whole number from 0 to 15");
				d.depth = static_cast<int>(v);
			}

			if (const auto it = j.find("triggers"); it != j.end()) {
				if (!it->is_object()) Fail("triggers", "expected an object");
				if (const auto h = it->find("hotkey"); h != it->end()) {
					long long v = 0;
					if (!AsInt(*h, v) || v < 2 || v > 255) Fail("triggers.hotkey", "expected a DirectInput scan code from 2 to 255 (1 is Escape)");
					d.hotkey = static_cast<int>(v);
				}
				d.papyrus = GetBool(*it, "papyrus", "triggers", true);
				if (const auto m = it->find("modEvent"); m != it->end()) {
					if (!m->is_string() || !IsValidModEventName(m->get_ref<const std::string&>())) Fail("triggers.modEvent", "must match ^[A-Za-z0-9_]{1,64}$");
					d.modEvent = m->get<std::string>();
				}
				NoteUnknown(*it, { "hotkey", "papyrus", "modEvent" }, "triggers", res.problems);
			}

			if (const auto it = j.find("extra"); it != j.end() && !it->is_object()) Fail("extra", "expected an object");
			NoteUnknown(j, { "specVersion", "name", "swf", "minRuntime", "flags", "inputContext", "depth", "triggers", "extra" }, "", res.problems);

			// S3 rule 4: minRuntime against the running version
			const auto required = ParseVersion(d.minRuntime);
			if (!required) Fail("minRuntime", "'" + d.minRuntime + "' is outside 0-255.0-255.0-4095");
			if (!IsCompatible(*required, have)) {
				error("ERROR menu '" + d.name + "' needs Inkwell Runtime " + d.minRuntime + ", this is " + ToString(have) + ": not registered");
				return res;
			}

			res.def = std::move(d);
		} catch (const Failure& f) {
			res.def.reset();
			error(f.text);
		} catch (const std::exception& e) {
			res.def.reset();
			error(std::string("unexpected error: ") + e.what());
		}
		return res;
	}
}
