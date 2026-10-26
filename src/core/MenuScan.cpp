#include "MenuScan.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>

namespace inkwell
{
	namespace fs = std::filesystem;

	namespace
	{
		std::string ReadAll(const fs::path& p, bool& ok)
		{
			std::ifstream in(p, std::ios::binary);
			ok = static_cast<bool>(in);
			std::stringstream ss;
			ss << in.rdbuf();
			return ss.str();
		}

		std::vector<fs::path> ListFiles(const fs::path& dir, std::string_view ext)
		{
			std::vector<fs::path> out;
			std::error_code       ec;
			for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
				std::error_code ec2;
				if (!it->is_regular_file(ec2)) continue;
				if (LowerCopy(it->path().extension().string()) != ext) continue;
				out.push_back(it->path());
			}
			// case-insensitive file-name order (S1 rule 2)
			std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) { return LowerCopy(a.filename().string()) < LowerCopy(b.filename().string()); });
			return out;
		}

		std::string TrimCopy(std::string_view s)
		{
			while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
			while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
			return std::string(s);
		}
	}

	ScanResult ScanMenus(const fs::path& baseDir)
	{
		ScanResult r;
		const auto msg = [&](std::string where, Problem::Level l, std::string text) { r.messages.push_back({ std::move(where), { l, std::move(text) } }); };

		// 1. per-mod INIs: <modId>.ini (Inkwell.ini is the runtime-wide file)
		std::vector<std::pair<fs::path, std::shared_ptr<const ParsedIni>>> inis;
		for (const auto& p : ListFiles(baseDir, ".ini")) {
			if (LowerCopy(p.filename().string()) == "inkwell.ini") continue;
			bool       ok = false;
			const auto text = ReadAll(p, ok);
			if (!ok) {
				msg(p.string(), Problem::Level::kWarn, "cannot be read; ignored");
				continue;
			}
			auto parsed = std::make_shared<ParsedIni>(ParseIni(text, IniKind::kMod));
			for (const auto& pr : parsed->problems) msg(p.string() + ":" + std::to_string(pr.line), Problem::Level::kWarn, "[" + pr.section + "] " + pr.key + ": " + pr.message);
			msg(p.string(), Problem::Level::kDebug, std::to_string(parsed->menus.size()) + " menu section(s)");
			inis.emplace_back(p, std::move(parsed));
		}

		// 2. menu JSON
		const fs::path  dir = baseDir / "menus";
		std::error_code ec;
		if (!fs::is_directory(dir, ec)) {
			r.dirMissing = true;
			return r;
		}
		std::map<std::string, std::string> byName;  // lower-case name -> file
		for (const auto& p : ListFiles(dir, ".json")) {
			++r.filesSeen;
			const std::string file = p.string();
			bool              ok = false;
			const auto        text = ReadAll(p, ok);
			if (!ok) {
				msg(file, Problem::Level::kError, "skipped, cannot be read");
				r.failedFiles.insert(LowerCopy(file));
				continue;
			}
			auto pr = ParseMenuDef(text);
			for (const auto& x : pr.problems) msg(file, x.level, x.level == Problem::Level::kError ? "skipped, " + x.message : x.message);
			if (!pr.def) {
				r.failedFiles.insert(LowerCopy(file));
				continue;
			}
			MenuDef&          d = *pr.def;
			const std::string key = MenuNameKey(d.name);
			if (const auto it = byName.find(key); it != byName.end()) {
				msg(file, Problem::Level::kError, "skipped, menu name '" + d.name + "' is already used by " + it->second);
				r.failedFiles.insert(LowerCopy(file));
				continue;
			}
			byName.emplace(key, file);

			MenuCandidate c;
			c.def = std::move(d);
			c.file = file;
			// the INI with this menu's section; otherwise <modId>.ini from the swf path Interface/Inkwell/<modId>/...
			for (const auto& [ip, parsed] : inis) {
				if (parsed->FindMenu(c.def.name)) {
					c.modIni = parsed;
					c.iniFile = ip.string();
					break;
				}
			}
			if (!c.modIni) {
				const std::string& bare = c.def.bareSwf;
				if (bare.size() > 8 && LowerCopy(bare.substr(0, 8)) == "inkwell/") {
					const auto slash = bare.find('/', 8);
					if (slash != std::string::npos) {
						const std::string want = LowerCopy(bare.substr(8, slash - 8)) + ".ini";
						for (const auto& [ip, parsed] : inis) {
							if (LowerCopy(ip.filename().string()) == want) {
								c.modIni = parsed;
								c.iniFile = ip.string();
								break;
							}
						}
					}
				}
			}
			c.eff = ResolveMenu(c.def.hotkey, c.modIni ? c.modIni->FindMenu(c.def.name) : nullptr);
			r.menus.push_back(std::move(c));
		}
		return r;
	}

	FileStamp StampOf(const fs::path& p)
	{
		FileStamp       s;
		std::error_code ec;
		s.size = fs::file_size(p, ec);
		if (ec) return s;
		const auto t = fs::last_write_time(p, ec);
		if (ec) return s;
		s.mtime = static_cast<long long>(t.time_since_epoch().count());
		s.exists = true;
		return s;
	}

	std::string UpsertIniValue(std::string_view text, std::string_view section, std::string_view key, std::string_view value)
	{
		const std::string        eol = text.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
		std::vector<std::string> lines;
		{
			std::string cur;
			for (char ch : text) {
				if (ch == '\n') {
					if (!cur.empty() && cur.back() == '\r') cur.pop_back();
					lines.push_back(cur);
					cur.clear();
				} else {
					cur.push_back(ch);
				}
			}
			if (!cur.empty()) lines.push_back(cur);
		}
		const std::string wantSec = LowerCopy(section), wantKey = LowerCopy(key);
		bool              inSec = false;
		long              lastKeyLine = -1, secEnd = -1;
		for (std::size_t i = 0; i < lines.size(); ++i) {
			const std::string t = TrimCopy(lines[i]);
			if (t.size() >= 2 && t.front() == '[' && t.back() == ']') {
				inSec = LowerCopy(TrimCopy(std::string_view(t).substr(1, t.size() - 2))) == wantSec;
				if (inSec) secEnd = static_cast<long>(i);
				continue;
			}
			if (!inSec) continue;
			if (!t.empty()) secEnd = static_cast<long>(i);
			if (t.empty() || t[0] == ';' || t[0] == '#') continue;
			const auto eq = t.find('=');
			if (eq != std::string::npos && LowerCopy(TrimCopy(std::string_view(t).substr(0, eq))) == wantKey) lastKeyLine = static_cast<long>(i);  // last duplicate wins
		}
		const std::string line = std::string(key) + "=" + std::string(value);
		if (lastKeyLine >= 0) lines[static_cast<std::size_t>(lastKeyLine)] = line;
		else if (secEnd >= 0) lines.insert(lines.begin() + secEnd + 1, line);
		else {
			if (!lines.empty() && !TrimCopy(lines.back()).empty()) lines.push_back("");
			lines.push_back("[" + std::string(section) + "]");
			lines.push_back(line);
		}
		std::string out;
		for (const auto& l : lines) out += l + eol;
		return out;
	}
}
