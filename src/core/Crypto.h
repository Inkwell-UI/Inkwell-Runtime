// Small crypto helpers for the dev server (S6): SHA-1 + base64 for the RFC 6455 handshake, constant-time compare, CSPRNG token.
// No engine headers.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace inkwell
{
	std::array<std::uint8_t, 20> Sha1(std::string_view data);
	std::string                  Base64(const std::uint8_t* data, std::size_t size);
	std::string                  HexLower(const std::uint8_t* data, std::size_t size);

	/// Compares without an early exit on the first differing byte (the length is not secret: tokens have a fixed length).
	bool ConstantTimeEqual(std::string_view a, std::string_view b);

	/// OS CSPRNG (BCryptGenRandom). False on failure.
	bool RandomBytes(std::uint8_t* out, std::size_t size);

	/// 64 lower-case hex digits from 32 random bytes; empty string if the CSPRNG failed.
	std::string GenerateToken();

	/// 64 lower-case hex digits (the S2/S6 token shape).
	bool IsValidToken(std::string_view t);

	/// `abcd…wxyz` (first 4 and last 4, O5); never the whole token.
	std::string MaskToken(std::string_view token);
}
