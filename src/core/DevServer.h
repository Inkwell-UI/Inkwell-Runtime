// S6 dev server: a WebSocket server on 127.0.0.1 with its own thread (Winsock, non-blocking, select). Engine-free: the plugin (and the test
// harness) talk to it through DevHost and the Send* methods, which are safe to call from any thread and never block on the network.
#pragma once
#include "DevProtocol.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace inkwell::dev
{
	/// Callbacks into the host. They run on the server thread: the host must only queue work (to the game thread) and return.
	class DevHost
	{
	public:
		virtual ~DevHost() = default;
		virtual void Log(int level /*0 trace .. 4 error*/, const std::string& text) = 0;
		virtual void OnClient(bool connected) = 0;
		/// `menu` is the canonical registered name. `watchWasOff` = watch was just switched on (the host asks the menu for a variable dump).
		virtual void OnSubscribe(const std::string& menu, bool watch, bool pins, bool watchWasOff) = 0;
		virtual void OnReload(double id, const std::optional<std::string>& menu) = 0;
	};

	struct ServerConfig
	{
		int         port{ kDefaultPort };  // 0 = pick a free port (tests)
		std::string token;                 // 64 lower-case hex digits
		std::uint32_t runtimeVersion{ 0 };
		std::string runtimeVersionString, gameVersion;
		int         protocolVersion{ 1 }, specVersion{ 6 };
		// overridable for tests
		int handshakeMs{ kHandshakeMs }, pingEveryMs{ kPingEveryMs }, pongTimeoutMs{ kPongTimeoutMs };
		std::size_t maxMessageBytes{ kMaxMessageBytes };
	};

	class DevServer
	{
	public:
		DevServer(DevHost& host, ServerConfig cfg);
		~DevServer();

		/// Binds 127.0.0.1:port and starts the thread. False with `err` filled (port in use, no Winsock).
		bool Start(std::string& err);
		void Stop();
		int  Port() const { return _port; }

		// ---- any thread; all are dropped quietly when no authenticated editor is connected ----
		bool Connected() const { return _authed; }
		void SetMenus(std::vector<MenuInfo> menus);
		void SendTrace(std::string_view level, std::string_view source, std::string_view menu, std::string_view node, std::string_view text);
		void SendMenuState(std::string_view menu, bool open);
		void SendWatch(std::string_view menu, std::string_view graph, std::string_view scope, std::string_view name, const Scalar& value);
		void SendPin(std::string_view menu, std::string_view node, std::string_view pin, const Scalar& value);
		void SendReloadAck(double id, bool ok, const std::vector<std::string>& reloaded, const std::vector<SkippedMenu>& skipped, std::int64_t ms, std::string_view error);
		void SendError(std::string_view code, std::string_view message, std::optional<double> id = std::nullopt);
		bool WantsWatch(std::string_view menu) const;
		bool WantsPins(std::string_view menu) const;

	private:
		struct Conn;
		void   Run();
		void   Handle(Conn& c, const std::string& text);
		void   Close(Conn& c, std::uint16_t code, const char* why);
		void   Queue(Conn& c, const std::string& frame);
		void   Log(int level, const std::string& s) { _host.Log(level, s); }
		std::int64_t Now() const;

		DevHost&     _host;
		ServerConfig _cfg;
		std::uintptr_t _listen{ ~std::uintptr_t{ 0 } };
		int          _port{ 0 };
		std::thread  _thread;
		std::atomic<bool> _stop{ false };
		std::atomic<bool> _authed{ false };
		std::int64_t _t0{ 0 };

		OutQueue      _out;
		StreamLimiter _stream;
		Lockout       _lockout;

		mutable std::mutex _m;  // _menus, _subs
		std::vector<MenuInfo> _menus;
		struct Sub { bool watch{ false }, pins{ false }; };
		std::map<std::string, Sub> _subs;  // lower-case menu name

		std::vector<std::unique_ptr<Conn>> _conns;  // server thread only
	};
}
