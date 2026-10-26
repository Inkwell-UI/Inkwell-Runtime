#include "Translations.h"

#include "Registry.h"  // LowerCopy

#include <fstream>
#include <iterator>
#include <utility>

namespace inkwell
{
	namespace
	{
		bool IsAlnum(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

		void AppendUtf8(std::string& out, std::uint32_t cp)
		{
			if (cp < 0x80) out += static_cast<char>(cp);
			else if (cp < 0x800) {
				out += static_cast<char>(0xC0 | (cp >> 6));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			} else if (cp < 0x10000) {
				out += static_cast<char>(0xE0 | (cp >> 12));
				out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			} else {
				out += static_cast<char>(0xF0 | (cp >> 18));
				out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
				out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
				out += static_cast<char>(0x80 | (cp & 0x3F));
			}
		}

		/// UTF-16 (either byte order) to UTF-8. A lone surrogate becomes U+FFFD; an odd trailing byte is dropped.
		std::string Utf16ToUtf8(const std::uint8_t* d, std::size_t n, bool bigEndian)
		{
			std::string out;
			out.reserve(n);
			const auto unit = [&](std::size_t i) -> std::uint32_t {
				return bigEndian ? (static_cast<std::uint32_t>(d[i]) << 8 | d[i + 1]) : (static_cast<std::uint32_t>(d[i + 1]) << 8 | d[i]);
			};
			for (std::size_t i = 0; i + 1 < n; i += 2) {
				std::uint32_t u = unit(i);
				if (u >= 0xD800 && u <= 0xDBFF) {
					if (i + 3 < n && unit(i + 2) >= 0xDC00 && unit(i + 2) <= 0xDFFF) {
						u = 0x10000 + ((u - 0xD800) << 10) + (unit(i + 2) - 0xDC00);
						i += 2;
					} else u = 0xFFFD;
				} else if (u >= 0xDC00 && u <= 0xDFFF) u = 0xFFFD;
				AppendUtf8(out, u);
			}
			return out;
		}

		std::string Unescape(std::string_view s)
		{
			std::string out;
			out.reserve(s.size());
			for (std::size_t i = 0; i < s.size(); ++i) {
				if (s[i] == '\\' && i + 1 < s.size()) {
					++i;
					out += (s[i] == 'n') ? '\n' : s[i];
				} else out += s[i];
			}
			return out;
		}
	}

	bool IsValidTranslationKey(std::string_view key)
	{
		if (key.size() < 2 || key.size() > 97 || key[0] != '$') return false;
		for (std::size_t i = 1; i < key.size(); ++i)
			if (!IsAlnum(key[i]) && key[i] != '_') return false;
		return true;
	}

	bool IsValidTranslationMod(std::string_view mod)
	{
		if (mod.empty() || mod.size() > 64 || !((mod[0] >= 'a' && mod[0] <= 'z') || (mod[0] >= 'A' && mod[0] <= 'Z'))) return false;
		for (const char c : mod)
			if (!IsAlnum(c) && c != '_') return false;
		return true;
	}

	bool IsValidLanguage(std::string_view language)
	{
		if (language.size() < 3 || language.size() > 20) return false;
		for (const char c : language)
			if (c < 'A' || c > 'Z') return false;
		return true;
	}

	ParsedTranslations ParseTranslationFile(const std::uint8_t* d, std::size_t n)
	{
		ParsedTranslations r;
		std::string        text;
		if (n >= 2 && d[0] == 0xFF && d[1] == 0xFE) {
			r.encoding = TextEncoding::kUtf16le;
			text = Utf16ToUtf8(d + 2, n - 2, false);
		} else if (n >= 2 && d[0] == 0xFE && d[1] == 0xFF) {
			r.encoding = TextEncoding::kUtf16be;
			text = Utf16ToUtf8(d + 2, n - 2, true);
		} else {
			const std::size_t start = (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) ? 3 : 0;
			text.assign(reinterpret_cast<const char*>(d) + start, n - start);
		}

		const auto issue = [&](int line, std::string msg) {
			r.issueLines.push_back(line);
			r.problems.push_back({ Problem::Level::kWarn, "line " + std::to_string(line) + ": " + std::move(msg) });
		};

		int         line = 0;
		std::size_t pos = 0;
		while (pos <= text.size()) {
			std::size_t end = pos;
			while (end < text.size() && text[end] != '\r' && text[end] != '\n') ++end;
			const std::string_view raw(text.data() + pos, end - pos);
			++line;
			if (!raw.empty() && raw[0] != ';' && raw[0] != '#') {
				const auto tab = raw.find('\t');
				if (tab == std::string_view::npos) issue(line, "no TAB between key and text");
				else {
					const std::string key(raw.substr(0, tab));
					if (!IsValidTranslationKey(key)) issue(line, "invalid key '" + key.substr(0, 40) + "'");
					else {
						if (r.entries.contains(key)) issue(line, "duplicate key " + key + ", the later line wins");
						r.entries[key] = Unescape(raw.substr(tab + 1));
					}
				}
			}
			if (end >= text.size()) break;
			pos = end + ((text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n') ? 2 : 1);
		}
		return r;
	}

	void TranslationStore::SetDirectory(std::filesystem::path dir)
	{
		std::lock_guard lk(_m);
		_dir = std::move(dir);
		_tables.clear();
	}

	const TranslationStore::Table& TranslationStore::Load(std::string_view mod, std::string_view language)
	{
		const std::string id = LowerCopy(mod) + "|" + std::string(language);
		auto&             t = _tables[id];
		if (t.loaded) return t;
		t.loaded = true;
		if (!IsValidTranslationMod(mod) || !IsValidLanguage(language)) return t;

		const auto path = _dir / (std::string(mod) + "_" + std::string(language) + ".txt");
		std::error_code ec;
		if (!std::filesystem::is_regular_file(path, ec)) return t;
		std::ifstream f(path, std::ios::binary);
		if (!f) {
			_problems.push_back({ Problem::Level::kWarn, "cannot open " + path.string() });
			return t;
		}
		const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		const auto                      parsed = ParseTranslationFile(bytes.data(), bytes.size());
		for (const auto& [k, v] : parsed.entries) t.byLowerKey[LowerCopy(k)] = v;
		for (const auto& p : parsed.problems) _problems.push_back({ p.level, path.filename().string() + ": " + p.message });
		_problems.push_back({ Problem::Level::kDebug, "loaded " + path.filename().string() + ": " + std::to_string(parsed.entries.size()) + " entries, " +
														  (parsed.encoding == TextEncoding::kUtf8 ? "UTF-8" : parsed.encoding == TextEncoding::kUtf16le ? "UTF-16 LE" : "UTF-16 BE") });
		return t;
	}

	std::optional<std::string> TranslationStore::Find(std::string_view mod, std::string_view language, std::string_view key)
	{
		std::lock_guard lk(_m);
		const auto&     t = Load(mod, language);
		const auto      it = t.byLowerKey.find(LowerCopy(key));
		if (it == t.byLowerKey.end()) return std::nullopt;
		return it->second;
	}

	std::vector<Problem> TranslationStore::TakeProblems()
	{
		std::lock_guard lk(_m);
		return std::exchange(_problems, {});
	}

	void TranslationStore::Clear()
	{
		std::lock_guard lk(_m);
		_tables.clear();
	}
}
