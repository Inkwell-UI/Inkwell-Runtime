// S5 saved variables: the in-memory store, the limits and the co-save record codec. Pure C++ (no engine), unit-tested.
// The plugin (src/plugin/Saved.cpp) only moves the encoded bytes in and out of the SKSE serialization interface.
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace inkwell
{
	// Inkwell-Spec runtime.ts COSAVE_* and SAVED_LIMITS
	inline constexpr std::uint32_t kCosaveUniqueId = 0x494E4B57;     // 'INKW'
	inline constexpr std::uint32_t kCosaveRecordSaved = 0x53564152;  // 'SVAR'
	inline constexpr std::uint32_t kCosaveRecordVersion = 1;
	inline constexpr std::size_t   kSavedVariablesPerMenu = 256, kSavedKeyBytes = 64, kSavedStringBytes = 4096;

	/// Alternative index = the type byte of the record (0 Bool, 1 Number, 2 String).
	using SavedValue = std::variant<bool, double, std::string>;
	using SavedVars = std::map<std::string, SavedValue>;  // variable name -> value

	enum class SetResult { kOk, kTruncated, kBadKey, kTooMany };

	/// menu key (lower-cased menu name, see MenuNameKey) -> variables. Thread-safe.
	class SavedStore
	{
	public:
		/// Strings over the limit are cut (on a UTF-8 boundary) and reported as kTruncated; the value is still stored.
		SetResult                 Set(std::string_view menuKey, std::string_view name, SavedValue value);
		std::optional<SavedValue> Get(std::string_view menuKey, std::string_view name) const;
		void                      Clear();
		/// Replaces the variables of one menu (used by the load callback).
		void                             Replace(std::string menuKey, SavedVars vars);
		std::map<std::string, SavedVars> Snapshot() const;

	private:
		mutable std::mutex               _m;
		std::map<std::string, SavedVars> _menus;
	};

	/// One 'SVAR' v1 record body (S5 rule 2). Entries that break the limits are dropped, so the output always decodes.
	std::vector<std::uint8_t> EncodeSavedRecord(const std::string& menu, const SavedVars& vars);

	struct DecodedRecord
	{
		std::string menu;
		SavedVars   vars;
	};

	/// nullopt + `error` for a truncated body, an unknown type byte, a bad key or a limit breach. Never throws, never reads out of range.
	std::optional<DecodedRecord> DecodeSavedRecord(const std::uint8_t* data, std::size_t size, std::string& error);

	/// Upper bound for a record body; a larger length field in the file is treated as corrupt and skipped unread.
	inline constexpr std::size_t kSavedRecordMaxBytes = 4 + 4096 + 4 + kSavedVariablesPerMenu * (4 + kSavedKeyBytes + 1 + 4 + kSavedStringBytes);
}
