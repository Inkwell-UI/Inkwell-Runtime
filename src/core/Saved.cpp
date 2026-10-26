#include "Saved.h"

#include <cstring>

namespace inkwell
{
	namespace
	{
		constexpr std::size_t kMenuNameMax = 4096;

		void PutU32(std::vector<std::uint8_t>& o, std::uint32_t v)
		{
			for (int i = 0; i < 4; ++i) o.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
		}
		void PutBytes(std::vector<std::uint8_t>& o, std::string_view s)
		{
			PutU32(o, static_cast<std::uint32_t>(s.size()));
			o.insert(o.end(), s.begin(), s.end());
		}

		struct Reader
		{
			const std::uint8_t* p;
			std::size_t         left;
			bool                ok{ true };

			bool Take(void* dst, std::size_t n)
			{
				if (!ok || n > left) return ok = false;
				if (n) std::memcpy(dst, p, n);
				p += n;
				left -= n;
				return true;
			}
			std::uint32_t U32()
			{
				std::uint8_t b[4]{};
				Take(b, 4);
				return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) | (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
			}
			// length-prefixed bytes; the length is checked against `max` and what is left before anything is allocated
			bool Str(std::string& out, std::size_t max, bool allowEmpty)
			{
				const auto n = U32();
				if (!ok || n > max || n > left || (n == 0 && !allowEmpty)) return ok = false;
				out.assign(reinterpret_cast<const char*>(p), n);
				p += n;
				left -= n;
				return true;
			}
		};
	}

	SetResult SavedStore::Set(std::string_view menuKey, std::string_view name, SavedValue value)
	{
		if (name.empty() || name.size() > kSavedKeyBytes) return SetResult::kBadKey;
		auto result = SetResult::kOk;
		if (auto* s = std::get_if<std::string>(&value); s && s->size() > kSavedStringBytes) {
			std::size_t n = kSavedStringBytes;
			while (n > 0 && (static_cast<unsigned char>((*s)[n]) & 0xC0) == 0x80) --n;  // do not cut a UTF-8 sequence in half
			s->resize(n);
			result = SetResult::kTruncated;
		}
		std::lock_guard lk(_m);
		auto&           vars = _menus[std::string(menuKey)];
		const auto      it = vars.find(std::string(name));
		if (it == vars.end() && vars.size() >= kSavedVariablesPerMenu) return SetResult::kTooMany;
		vars.insert_or_assign(std::string(name), std::move(value));
		return result;
	}

	std::optional<SavedValue> SavedStore::Get(std::string_view menuKey, std::string_view name) const
	{
		std::lock_guard lk(_m);
		const auto      m = _menus.find(std::string(menuKey));
		if (m == _menus.end()) return std::nullopt;
		const auto v = m->second.find(std::string(name));
		if (v == m->second.end()) return std::nullopt;
		return v->second;
	}

	void SavedStore::Clear()
	{
		std::lock_guard lk(_m);
		_menus.clear();
	}

	void SavedStore::Replace(std::string menuKey, SavedVars vars)
	{
		std::lock_guard lk(_m);
		_menus[std::move(menuKey)] = std::move(vars);
	}

	std::map<std::string, SavedVars> SavedStore::Snapshot() const
	{
		std::lock_guard lk(_m);
		return _menus;
	}

	std::vector<std::uint8_t> EncodeSavedRecord(const std::string& menu, const SavedVars& vars)
	{
		std::vector<std::uint8_t> out;
		PutBytes(out, menu);
		const auto countPos = out.size();  // patched after the loop, because invalid entries are skipped
		PutU32(out, 0);
		std::uint32_t count = 0;
		for (const auto& [name, v] : vars) {
			if (name.empty() || name.size() > kSavedKeyBytes || count >= kSavedVariablesPerMenu) continue;
			PutBytes(out, name);
			out.push_back(static_cast<std::uint8_t>(v.index()));
			if (const auto* b = std::get_if<bool>(&v)) {
				out.push_back(*b ? 1 : 0);
			} else if (const auto* d = std::get_if<double>(&v)) {
				std::uint64_t bits;
				std::memcpy(&bits, d, 8);
				for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
			} else {
				PutBytes(out, std::string_view(std::get<std::string>(v)).substr(0, kSavedStringBytes));
			}
			++count;
		}
		for (int i = 0; i < 4; ++i) out[countPos + i] = static_cast<std::uint8_t>(count >> (8 * i));
		return out;
	}

	std::optional<DecodedRecord> DecodeSavedRecord(const std::uint8_t* data, std::size_t size, std::string& error)
	{
		try {
			if (!data && size) {
				error = "null buffer";
				return std::nullopt;
			}
			Reader        r{ data, size };
			DecodedRecord out;
			if (!r.Str(out.menu, kMenuNameMax, false)) {
				error = "truncated or invalid menu name";
				return std::nullopt;
			}
			const auto count = r.U32();
			if (!r.ok || count > kSavedVariablesPerMenu) {
				error = "bad variable count";
				return std::nullopt;
			}
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto fail = [&](const char* why) {
					error = "entry " + std::to_string(i) + ": " + why;
					return std::nullopt;
				};
				std::string key;
				if (!r.Str(key, kSavedKeyBytes, false)) return fail("bad key");
				std::uint8_t type = 0xFF;
				if (!r.Take(&type, 1)) return fail("truncated");
				if (type == 0) {
					std::uint8_t b = 0;
					if (!r.Take(&b, 1)) return fail("truncated bool");
					out.vars[key] = b != 0;
				} else if (type == 1) {
					std::uint8_t b[8];
					if (!r.Take(b, 8)) return fail("truncated number");
					std::uint64_t bits = 0;
					for (int k = 0; k < 8; ++k) bits |= static_cast<std::uint64_t>(b[k]) << (8 * k);
					double d;
					std::memcpy(&d, &bits, 8);
					out.vars[key] = d;
				} else if (type == 2) {
					std::string s;
					if (!r.Str(s, kSavedStringBytes, true)) return fail("bad string");
					out.vars[key] = std::move(s);
				} else {
					return fail("unknown type byte");
				}
			}
			if (r.left != 0) error = std::to_string(r.left) + " trailing byte(s) ignored";  // not fatal
			return out;
		} catch (...) {
			error = "exception while decoding";
			return std::nullopt;
		}
	}
}
