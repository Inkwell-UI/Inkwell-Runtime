// Reads Data/SKSE/Plugins/Inkwell/menus/*.json and the mod INIs into validated candidates (S1, S2). Used at startup and by hot reload (S6),
// so both apply the same validation. Pure C++ (std::filesystem), no engine.
#pragma once
#include "Ini.h"
#include "MenuConfig.h"
#include "Problem.h"
#include "Registry.h"

#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace inkwell
{
	struct MenuCandidate
	{
		MenuDef                          def;
		EffectiveMenu                    eff;
		std::shared_ptr<const ParsedIni> modIni;  // the INI that holds this menu's section (or <modId>.ini); may be null
		std::string                      iniFile, file;
	};

	struct ScanMessage
	{
		std::string where;
		Problem     problem;
	};

	struct ScanResult
	{
		std::vector<MenuCandidate> menus;        // valid, unique by name, in file-name order
		std::vector<ScanMessage>   messages;     // everything the caller should log
		std::set<std::string>      failedFiles;  // lower-case paths of JSON files that exist but were rejected
		int                        filesSeen{ 0 };
		bool                       dirMissing{ false };
	};

	/// `baseDir` = Data/SKSE/Plugins/Inkwell. Never throws.
	ScanResult ScanMenus(const std::filesystem::path& baseDir);

	struct FileStamp
	{
		std::uintmax_t size{ 0 };
		long long      mtime{ 0 };
		bool           exists{ false };
		bool operator==(const FileStamp&) const = default;
	};
	FileStamp StampOf(const std::filesystem::path& p);

	/// Sets `[section] key=value` in INI text (replaces the key, or adds the key or the section; keeps the line endings and everything else).
	std::string UpsertIniValue(std::string_view text, std::string_view section, std::string_view key, std::string_view value);
}
