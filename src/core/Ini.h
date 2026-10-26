// S2 user INI: a C++ port of Inkwell-Spec/src/ini.ts (parseIni, resolveMenu). Tolerant, never throws.
// The shared test vectors in test-vectors/ini/ are the contract; tests/core_tests.cpp runs them.
#pragma once
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace inkwell
{
	inline constexpr double kUiScaleMin = 0.5, kUiScaleMax = 2.0;

	struct MenuIni  // [Menu.<name>]; every key optional (absent = not set by the user)
	{
		std::optional<int>         openKey;  // 0 = no hotkey, otherwise 2..255
		std::optional<double>      uiScale;
		std::optional<int>         offsetX, offsetY;
		std::optional<std::string> theme;
	};

	struct WidgetIni  // [Widget.<name>]: parsed and kept, not applied in 0.3 (O7)
	{
		std::optional<int>    offsetX, offsetY;
		std::optional<double> scale;
	};

	struct IniProblem
	{
		int         line{ 0 };
		std::string section, key, message;
	};

	enum class IniKind { kMod, kRuntime };

	struct ParsedIni
	{
		// Keyed by the name as first written in the file (insertion order kept); look up case-insensitively with FindMenu.
		std::vector<std::pair<std::string, MenuIni>>   menus;
		std::vector<std::pair<std::string, WidgetIni>> widgets;
		// Runtime-wide sections (only read from Inkwell.ini)
		std::optional<std::string> logLevel;
		std::optional<bool>        devMode, watch;
		std::optional<int>         port;
		std::optional<std::string> token;
		bool                       hasRuntimeSection{ false }, hasDevSection{ false };  // present with at least one valid key
		std::vector<IniProblem>    problems;
		// Every [section] key=value of the file, lower-cased, for `inkwell.getSetting` (not part of the Spec model).
		std::map<std::string, std::map<std::string, std::string>> raw;

		const MenuIni* FindMenu(std::string_view name) const;
	};

	ParsedIni ParseIni(std::string_view text, IniKind kind = IniKind::kMod);

	struct EffectiveMenu
	{
		std::optional<int> openKey;  // nullopt = no hotkey
		double             uiScale{ 1.0 };
		int                offsetX{ 0 }, offsetY{ 0 };
		std::string        theme;
	};

	/// Precedence: built-in default < author JSON < user INI. `openKey` 0 in the INI removes the hotkey.
	EffectiveMenu ResolveMenu(std::optional<int> jsonHotkey, const MenuIni* ini);
}
