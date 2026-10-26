#include "Crypto.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#	define WIN32_LEAN_AND_MEAN
#	define NOMINMAX
#	include <windows.h>
#	include <bcrypt.h>
#endif

namespace inkwell
{
	namespace
	{
		constexpr std::uint32_t Rol(std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
	}

	std::array<std::uint8_t, 20> Sha1(std::string_view data)
	{
		std::uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
		std::string   msg(data);
		const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
		msg.push_back(static_cast<char>(0x80));
		while (msg.size() % 64 != 56) msg.push_back('\0');
		for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bits >> (i * 8)) & 0xFF));

		for (std::size_t off = 0; off < msg.size(); off += 64) {
			std::uint32_t w[80];
			for (int i = 0; i < 16; ++i) {
				const auto* p = reinterpret_cast<const std::uint8_t*>(msg.data() + off + i * 4);
				w[i] = (std::uint32_t{ p[0] } << 24) | (std::uint32_t{ p[1] } << 16) | (std::uint32_t{ p[2] } << 8) | p[3];
			}
			for (int i = 16; i < 80; ++i) w[i] = Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
			std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
			for (int i = 0; i < 80; ++i) {
				std::uint32_t f, k;
				if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
				else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
				else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
				else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
				const std::uint32_t t = Rol(a, 5) + f + e + k + w[i];
				e = d; d = c; c = Rol(b, 30); b = a; a = t;
			}
			h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
		}
		std::array<std::uint8_t, 20> out{};
		for (int i = 0; i < 5; ++i) {
			out[i * 4] = static_cast<std::uint8_t>(h[i] >> 24);
			out[i * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
			out[i * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
			out[i * 4 + 3] = static_cast<std::uint8_t>(h[i]);
		}
		return out;
	}

	std::string Base64(const std::uint8_t* d, std::size_t n)
	{
		static constexpr char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string           out;
		for (std::size_t i = 0; i < n; i += 3) {
			const std::uint32_t v = (std::uint32_t{ d[i] } << 16) | (i + 1 < n ? std::uint32_t{ d[i + 1] } << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
			out.push_back(t[(v >> 18) & 63]);
			out.push_back(t[(v >> 12) & 63]);
			out.push_back(i + 1 < n ? t[(v >> 6) & 63] : '=');
			out.push_back(i + 2 < n ? t[v & 63] : '=');
		}
		return out;
	}

	std::string HexLower(const std::uint8_t* d, std::size_t n)
	{
		static constexpr char t[] = "0123456789abcdef";
		std::string           out;
		for (std::size_t i = 0; i < n; ++i) {
			out.push_back(t[d[i] >> 4]);
			out.push_back(t[d[i] & 15]);
		}
		return out;
	}

	bool ConstantTimeEqual(std::string_view a, std::string_view b)
	{
		// Always walks the longer length; a length difference is folded into the result.
		const std::size_t n = std::max(a.size(), b.size());
		unsigned          diff = static_cast<unsigned>(a.size() ^ b.size());
		for (std::size_t i = 0; i < n; ++i) {
			const unsigned x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0u;
			const unsigned y = i < b.size() ? static_cast<unsigned char>(b[i]) : 0u;
			diff |= x ^ y;
		}
		return diff == 0;
	}

	bool RandomBytes(std::uint8_t* out, std::size_t size)
	{
#if defined(_WIN32)
		return BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
		(void)out; (void)size;
		return false;
#endif
	}

	std::string GenerateToken()
	{
		std::uint8_t b[32];
		if (!RandomBytes(b, sizeof(b))) return {};
		return HexLower(b, sizeof(b));
	}

	bool IsValidToken(std::string_view t)
	{
		return t.size() == 64 && std::all_of(t.begin(), t.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
	}

	std::string MaskToken(std::string_view t)
	{
		if (t.size() < 12) return "(short token)";
		return std::string(t.substr(0, 4)) + "\xE2\x80\xA6" + std::string(t.substr(t.size() - 4));
	}
}
