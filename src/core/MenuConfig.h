// S1 menu JSON: validation that mirrors the Spec's `MenuDef` (Inkwell-Spec/src/menu.ts) field by field.
#pragma once
#include "Problem.h"
#include "Version.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace inkwell
{
	inline constexpr std::size_t MENU_SLOTS = 16;  // Spec MENU_SLOTS
	inline constexpr int         kMenuMinSpecVersion = 6;

	enum class InputContext { kNone, kMenuMode };

	struct MenuDef
	{
		int         specVersion{ 0 };
		std::string name;
		std::string swf;      // as written in the file
		std::string bareSwf;  // what LoadMovieEx needs: "Interface/" and ".swf" stripped
		std::string minRuntime{ "0.3.0" };
		bool        pausesGame{ false };
		bool        cursor{ false };
		bool        modal{ false };
		bool        closeOnEscape{ true };
		InputContext inputContext{ InputContext::kNone };  // effective value (modal -> menuMode when absent)
		bool         inputContextExplicit{ false };
		int          depth{ 3 };
		std::optional<int> hotkey;  // JSON triggers.hotkey, DirectInput scan code
		bool         papyrus{ true };
		std::string  modEvent;  // empty = none
		std::string  translations;  // V13.2: mod name of Interface/Translations/<translations>_<LANGUAGE>.txt; empty = none
		bool operator==(const MenuDef&) const = default;
	};

	struct MenuParseResult
	{
		std::optional<MenuDef> def;  // empty = the file is skipped; `problems` holds the reason (an error)
		std::vector<Problem>   problems;
	};

	/// S1 `swf` rule: the bare movie name, or nullopt when the path is invalid. "Interface/Inkwell/MyMod/Wheel.swf" -> "Inkwell/MyMod/Wheel".
	std::optional<std::string> NormalizeSwfPath(std::string_view swf);

	bool IsValidMenuName(std::string_view name);  // ^[A-Za-z][A-Za-z0-9_]{1,63}$
	bool IsValidModEventName(std::string_view s);  // ^[A-Za-z0-9_]{1,64}$

	/// Parses and validates one menu file's text. Never throws. The first invalid field skips the whole file (S1 rule 1).
	/// `have` is the running runtime version, used for the `minRuntime` check (S3 rule 4).
	MenuParseResult ParseMenuDef(std::string_view jsonText, const Version& have = kRuntimeVersion);

	/// Lower-case key for the case-insensitive uniqueness of menu names.
	std::string MenuNameKey(std::string_view name);
}
