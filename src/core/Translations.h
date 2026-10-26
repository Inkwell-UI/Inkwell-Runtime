// V13.2 translations: the reader for `Interface/Translations/<Mod>_<LANGUAGE>.txt` and the per-mod store behind `inkwell.translate(key)`.
// Skyrim only loads these files for loaded plugins, and Inkwell mods have none, so the runtime reads them itself. Pure C++ (no engine), unit-tested.
// ParseTranslationFile mirrors Inkwell-Spec `parseTranslationFile` (src/localization.ts); `test-vectors/translations/` keeps the two in step.
#pragma once
#include "Problem.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace inkwell
{
	enum class TextEncoding { kUtf8, kUtf16le, kUtf16be };

	struct ParsedTranslations
	{
		std::map<std::string, std::string> entries;  // key as written (`$Key`) -> UTF-8 text with real line breaks
		TextEncoding                       encoding{ TextEncoding::kUtf8 };
		std::vector<int>                   issueLines;  // 1-based lines that were skipped or overwritten (same lines as the Spec's `issues`)
		std::vector<Problem>               problems;    // the same, as messages
	};

	/// `$` + 1 to 96 letters, digits or `_`.
	bool IsValidTranslationKey(std::string_view key);
	/// ^[A-Za-z][A-Za-z0-9_]{0,63}$ (the mod name in the file name and in the menu JSON `translations`).
	bool IsValidTranslationMod(std::string_view mod);
	/// ^[A-Z]{3,20}$
	bool IsValidLanguage(std::string_view language);

	/// BOM decides: FF FE = UTF-16 LE, FE FF = UTF-16 BE, EF BB BF or none = UTF-8. Lines end CRLF, LF or CR; blank lines and lines starting `;` or `#` are
	/// skipped; a `$KEY`, a TAB, then the text; the first TAB splits; the two characters backslash-n are a line break and a doubled backslash one backslash.
	/// A later duplicate key wins. Never throws; malformed lines are problems.
	ParsedTranslations ParseTranslationFile(const std::uint8_t* data, std::size_t size);

	/// Translations per mod and language, read lazily from `<dir>/<Mod>_<LANGUAGE>.txt` and kept for the session (a missing file is remembered as empty).
	/// Lookups ignore the case of the key (the engine does too). Thread-safe.
	class TranslationStore
	{
	public:
		void SetDirectory(std::filesystem::path dir);
		/// The text for `key`, or nullopt when the file or the key does not exist.
		std::optional<std::string> Find(std::string_view mod, std::string_view language, std::string_view key);
		/// Problems found while loading files since the last call (file read errors, malformed lines); the plugin logs them.
		std::vector<Problem> TakeProblems();
		/// Forget everything loaded (hot reload in dev mode).
		void Clear();

	private:
		struct Table
		{
			bool                               loaded{ false };
			std::map<std::string, std::string> byLowerKey;
		};
		const Table& Load(std::string_view mod, std::string_view language);

		std::mutex                    _m;
		std::filesystem::path         _dir;
		std::map<std::string, Table>  _tables;  // "<lower mod>|<LANGUAGE>"
		std::vector<Problem>          _problems;
	};
}
