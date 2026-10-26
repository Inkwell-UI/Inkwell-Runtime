// S6 debug protocol, C++ side: the JSON message codec (mirrors Inkwell-Spec/src/runtimeProtocol.ts, RUNTIME_PROTOCOL 1), the bounded outgoing
// queue and the stream limiter. Pure C++, no sockets, no engine.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace inkwell::dev
{
	// Spec RUNTIME_LIMITS
	inline constexpr std::size_t kMaxMessageBytes = 1024 * 1024;
	inline constexpr int         kHandshakeMs = 5000, kPingEveryMs = 10'000, kPongTimeoutMs = 30'000, kDefaultPort = 8788;
	inline constexpr std::size_t kQueueSize = 1000;
	inline constexpr int         kPinsPerSecondPerPin = 20, kMessagesPerSecond = 200, kPinValueChars = 512, kFailedTokens = 5, kLockoutMs = 60'000;

	// ---- editor -> runtime ------------------------------------------------------------------------------------------
	struct Hello { std::string token, editorVersion; int protocolVersion{ 0 }, specVersion{ 0 }; };
	struct Subscribe { std::string menu; bool watch{ false }, pins{ false }, on{ true }; };  // on=false is `unsubscribe`
	struct Reload { double id{ 0 }; std::optional<std::string> menu; };
	struct Ping { std::optional<double> id; };
	struct Pong { std::optional<double> id; };
	struct Unknown { std::string type; };  // ignored (versioning rule: receivers ignore unknown types)
	using EditorMessage = std::variant<Hello, Subscribe, Reload, Ping, Pong, Unknown>;

	struct ParseOutcome
	{
		std::optional<EditorMessage> msg;
		std::string                  error;  // non-empty = `error bad-message` with this text
	};
	ParseOutcome ParseEditorMessage(std::string_view text);

	// ---- runtime -> editor (each returns the JSON text) ---------------------------------------------------------------
	struct MenuInfo { std::string name, swf; bool open{ false }; };
	struct HelloAckInfo
	{
		bool        ok{ true };
		std::string error;  // unauthorized, protocol-mismatch, busy
		std::uint32_t runtimeVersion{ 0 };
		std::string runtimeVersionString, gameVersion;
		int         protocolVersion{ 1 }, specVersion{ 6 };
		std::vector<MenuInfo> menus;
	};
	using Scalar = std::variant<std::monostate, bool, double, std::string>;  // monostate = null

	std::string MakeHelloAck(const HelloAckInfo& i);
	std::string MakeMenuState(std::string_view menu, bool open);
	std::string MakeTrace(std::int64_t ts, std::string_view level, std::string_view source, std::string_view menu, std::string_view node, std::string_view graph, std::string_view text);
	std::string MakeWatch(std::string_view menu, std::string_view graph, std::string_view scope, std::string_view name, const Scalar& value);
	std::string MakePin(std::string_view menu, std::string_view node, std::string_view pin, const Scalar& value);
	struct SkippedMenu { std::string name, reason; };
	std::string MakeReloadAck(double id, bool ok, const std::vector<std::string>& reloaded, const std::vector<SkippedMenu>& skipped, std::int64_t ms, std::string_view error);
	std::string MakeError(std::string_view code, std::string_view message, std::optional<double> id = std::nullopt);
	std::string MakePing(std::optional<double> id);
	std::string MakePong(std::optional<double> id);

	/// A pin value that is a string is cut to kPinValueChars characters (S6: Array/Object arrive as JSON strings).
	std::string CutPinText(std::string s);

	// ---- outgoing queue ---------------------------------------------------------------------------------------------
	/// Drop order when full (S6): pin, watch, other, trace, error.
	enum class Kind : int { kPin = 0, kWatch = 1, kControl = 2, kTrace = 3, kError = 4 };

	class OutQueue
	{
	public:
		explicit OutQueue(std::size_t cap = kQueueSize) :
			_cap(cap) {}
		/// Never blocks, never grows past the cap: drops the oldest message of the lowest kind present (counted).
		void Push(Kind k, std::string json);
		bool Pop(std::string& out);
		/// Returns and resets the number of messages dropped since the last call.
		std::uint64_t TakeDropped();
		std::size_t   Size() const;
		void          Clear();

	private:
		mutable std::mutex                       _m;
		std::deque<std::pair<Kind, std::string>> _q;
		std::size_t                              _cap;
		std::uint64_t                            _dropped{ 0 };
	};

	// ---- stream limiter ---------------------------------------------------------------------------------------------
	/// Coalesces to the latest value per key; releases at most `perKeyPerSec` per key and `totalPerSec` in total.
	class StreamLimiter
	{
	public:
		StreamLimiter(int perKeyPerSec = kPinsPerSecondPerPin, int totalPerSec = kMessagesPerSecond, std::size_t maxKeys = 4096) :
			_perKey(perKeyPerSec), _total(totalPerSec), _maxKeys(maxKeys) {}
		/// Stores the latest `json` for `key`. False when the key table is full (message dropped).
		bool Submit(const std::string& key, std::string json);
		/// Messages due at time `nowMs` (any steady clock in ms).
		std::vector<std::pair<std::string, std::string>> Flush(std::int64_t nowMs);  // (key, json)
		void                     Clear();

	private:
		struct Slot { std::string pending; bool has{ false }; std::int64_t lastSent{ -1'000'000 }; };
		std::mutex                  _m;
		std::map<std::string, Slot> _slots;
		int                         _perKey, _total;
		std::size_t                 _maxKeys;
		std::int64_t                _windowStart{ 0 };
		int                         _inWindow{ 0 };
	};

	// ---- failed-token lockout (S6 rule 4) ------------------------------------------------------------------------------
	class Lockout
	{
	public:
		void Fail(std::int64_t nowMs);
		bool Locked(std::int64_t nowMs) const { return nowMs < _until; }

	private:
		std::deque<std::int64_t> _fails;
		std::int64_t             _until{ 0 };
	};
}
