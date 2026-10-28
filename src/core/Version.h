// S3: runtime version, packed Int and the compatibility rule (same major, not lower). Pure C++, no engine headers.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#ifndef INKWELL_VERSION_MAJOR
#define INKWELL_VERSION_MAJOR 0
#define INKWELL_VERSION_MINOR 5
#define INKWELL_VERSION_PATCH 1
#endif

namespace inkwell
{
	/// Version of the menu/INI formats this runtime implements (Inkwell-Spec SPEC_VERSION).
	inline constexpr int kSpecVersion = 8;
	/// Version of the debug protocol (Inkwell-Spec RUNTIME_PROTOCOL). The server itself arrives in a later phase.
	inline constexpr int kRuntimeProtocol = 1;

	struct Version
	{
		int major{ 0 }, minor{ 0 }, patch{ 0 };
	};

	inline constexpr Version kRuntimeVersion{ INKWELL_VERSION_MAJOR, INKWELL_VERSION_MINOR, INKWELL_VERSION_PATCH };

	/// major*16777216 + minor*65536 + patch*16 (the same packing as SKSE's plugin version).
	constexpr std::uint32_t Pack(const Version& v)
	{
		return static_cast<std::uint32_t>(v.major) * 16777216u + static_cast<std::uint32_t>(v.minor) * 65536u + static_cast<std::uint32_t>(v.patch) * 16u;
	}

	inline std::string ToString(const Version& v)
	{
		return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
	}

	/// `MAJOR.MINOR.PATCH`, major and minor 0..255, patch 0..4095 (Spec parseVersion); anything else is nullopt.
	inline std::optional<Version> ParseVersion(std::string_view s)
	{
		Version v;
		int*    parts[3] = { &v.major, &v.minor, &v.patch };
		const std::size_t maxDigits[3] = { 3, 3, 4 };
		std::size_t       pos = 0;
		for (int i = 0; i < 3; ++i) {
			std::size_t digits = 0;
			long        n = 0;
			while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
				n = n * 10 + (s[pos] - '0');
				++pos;
				if (++digits > maxDigits[i]) return std::nullopt;
			}
			if (digits == 0) return std::nullopt;
			*parts[i] = static_cast<int>(n);
			if (i < 2) {
				if (pos >= s.size() || s[pos] != '.') return std::nullopt;
				++pos;
			}
		}
		if (pos != s.size() || v.major > 255 || v.minor > 255 || v.patch > 4095) return std::nullopt;
		return v;
	}

	/// O10: compatible = same major and the installed version is not lower than the required one.
	constexpr bool IsCompatible(const Version& required, const Version& have)
	{
		return required.major == have.major && Pack(have) >= Pack(required);
	}
}
