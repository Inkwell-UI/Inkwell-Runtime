// Two small thread-safe tables with no engine dependency, so they are unit-tested:
//  - DataStore: the last value per (menu, key) from Inkwell_API.SetString/SetNumber/SetBool (S4.1), replayed in set order.
//  - FunctionTable: the "Call Runtime" table (O1): built-ins plus functions other SKSE plugins register (S4.3).
#pragma once
#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace inkwell
{
	inline std::string LowerCopy(std::string_view s)
	{
		std::string r(s);
		std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return r;
	}

	using DataValue = std::variant<std::string, double, bool>;

	struct DataEntry
	{
		std::string key;
		DataValue   value;
	};

	class DataStore
	{
	public:
		/// Keeps the last value per (menu, key). A key set again moves to the end (replay is in the order of the last set).
		void Set(std::string_view menu, std::string key, DataValue value)
		{
			std::lock_guard lk(_m);
			auto&           v = _data[LowerCopy(menu)];
			std::erase_if(v, [&](const DataEntry& e) { return e.key == key; });
			v.push_back({ std::move(key), std::move(value) });
		}

		std::vector<DataEntry> Snapshot(std::string_view menu) const
		{
			std::lock_guard lk(_m);
			const auto      it = _data.find(LowerCopy(menu));
			return it == _data.end() ? std::vector<DataEntry>{} : it->second;
		}

	private:
		mutable std::mutex                           _m;
		std::map<std::string, std::vector<DataEntry>> _data;
	};

	/// `plugin.function`, lower-case: ^[a-z0-9_.]{3,64}$ (Spec MESSAGING_FUNCTION_NAME)
	inline bool IsValidFunctionName(std::string_view n)
	{
		if (n.size() < 3 || n.size() > 64) return false;
		return std::all_of(n.begin(), n.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.'; });
	}

	class FunctionTable
	{
	public:
		using Fn = std::function<std::string(const std::string& arg)>;

		/// Returns an empty string on success, otherwise the refusal reason.
		std::string Register(const std::string& name, Fn fn)
		{
			if (!IsValidFunctionName(name)) return "name must match ^[a-z0-9_.]{3,64}$";
			if (!fn) return "no function";
			std::lock_guard lk(_m);
			if (!_fns.emplace(name, std::move(fn)).second) return "name already registered";
			return {};
		}

		/// nullopt = unknown name (Call Runtime then returns "").
		std::optional<std::string> Call(std::string_view name, const std::string& arg) const
		{
			Fn fn;
			{
				std::lock_guard lk(_m);
				const auto      it = _fns.find(LowerCopy(name));
				if (it == _fns.end()) return std::nullopt;
				fn = it->second;
			}
			return fn(arg);
		}

		std::size_t Size() const
		{
			std::lock_guard lk(_m);
			return _fns.size();
		}

	private:
		mutable std::mutex   _m;
		std::map<std::string, Fn> _fns;
	};
}
