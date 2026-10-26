// Minimal RFC 6455 for the dev server (S6): HTTP upgrade check, frame reader/writer. Text, binary (rejected by the server), ping, pong, close,
// fragmentation. Pure functions and a byte-fed reader: no sockets, no engine, never throws, never trusts a length.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace inkwell::ws
{
	enum Opcode : std::uint8_t { kContinuation = 0, kText = 1, kBinary = 2, kClose = 8, kPing = 9, kPong = 10 };

	// Close codes used by the server (RFC 6455 7.4.1)
	inline constexpr std::uint16_t kCloseNormal = 1000, kCloseGoingAway = 1001, kCloseProtocol = 1002, kCloseUnsupported = 1003, kCloseBadData = 1007,
								   kClosePolicy = 1008, kCloseTooBig = 1009;

	enum class UpgradeStatus { kNeedMore, kOk, kBadRequest, kForbidden, kTooLarge };

	struct UpgradeResult
	{
		UpgradeStatus status{ UpgradeStatus::kNeedMore };
		std::size_t   consumed{ 0 };  // bytes of the request head; anything after is already WebSocket data
		std::string   key;            // Sec-WebSocket-Key when kOk
		std::string   reason;         // short text for the log
	};

	/// Parses an HTTP request head. S6: 403 when an `Origin` header is present, or when `Host` is not `127.0.0.1:<port>` / `localhost:<port>`.
	UpgradeResult ParseUpgrade(std::string_view buffer, int port);

	/// RFC 6455 4.2.2: base64(SHA-1(key + GUID)).
	std::string AcceptKey(std::string_view key);
	std::string UpgradeResponse(std::string_view key);
	std::string HttpError(int code, std::string_view reason);

	/// Unmasked frame (server to client).
	std::string EncodeFrame(std::uint8_t opcode, std::string_view payload, bool fin = true);
	/// Masked frame (client to server; tests and the harness).
	std::string EncodeMaskedFrame(std::uint8_t opcode, std::string_view payload, const std::uint8_t mask[4], bool fin = true);
	std::string EncodeClose(std::uint16_t code, std::string_view reason = {});

	bool IsValidUtf8(std::string_view s);

	struct Message
	{
		std::uint8_t opcode{ 0 };  // kText, kBinary, kClose, kPing, kPong (continuations are joined)
		std::string  payload;
	};

	/// Byte-fed frame reader. `expectMasked` = true on the server (client frames must be masked), false on a client.
	class Reader
	{
	public:
		enum class Status { kNeedMore, kMessage, kError };

		Reader(std::size_t maxMessageBytes, bool expectMasked) :
			_max(maxMessageBytes), _masked(expectMasked) {}

		void Feed(const char* data, std::size_t size) { _buf.append(data, size); }
		/// kMessage: a complete message or control frame is in `out`. kError: `CloseCode()` says why; the reader stays in error.
		Status        Next(Message& out);
		std::uint16_t CloseCode() const { return _code; }
		const char*   ErrorText() const { return _err; }

	private:
		Status Fail(std::uint16_t code, const char* why)
		{
			_code = code;
			_err = why;
			_failed = true;
			return Status::kError;
		}

		std::size_t   _max;
		bool          _masked;
		std::string   _buf;
		std::size_t   _pos{ 0 };
		std::string   _msg;         // fragmented data message being assembled
		std::uint8_t  _msgOp{ 0 };  // 0 = none in progress
		std::uint16_t _code{ 0 };
		const char*   _err{ "" };
		bool          _failed{ false };
	};
}
