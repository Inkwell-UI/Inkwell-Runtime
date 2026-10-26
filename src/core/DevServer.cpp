#include "DevServer.h"

#include "Crypto.h"
#include "WebSocket.h"

#include <algorithm>
#include <chrono>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

namespace inkwell::dev
{
	namespace
	{
		std::string Lower(std::string_view s)
		{
			std::string r(s);
			std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return r;
		}
		constexpr std::size_t kMaxConns = 8;            // unauthenticated peers time out after the handshake window
		constexpr std::size_t kOutSoftCap = 256 * 1024;  // stop pulling from the queue while this much is unsent (the queue then drops)
		constexpr std::size_t kOutHardCap = 8 * 1024 * 1024;
	}

	struct DevServer::Conn
	{
		SOCKET       s{ INVALID_SOCKET };
		std::string  in, out;
		ws::Reader   reader;
		bool         upgraded{ false }, primary{ false }, authed{ false }, closeAfterFlush{ false }, dead{ false };
		std::int64_t accepted{ 0 }, lastPong{ 0 }, lastPing{ 0 };
		double       pingId{ 0 };
		Conn(std::size_t max) :
			reader(max, true) {}
	};

	DevServer::DevServer(DevHost& host, ServerConfig cfg) :
		_host(host), _cfg(std::move(cfg)) {}

	DevServer::~DevServer() { Stop(); }

	std::int64_t DevServer::Now() const
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	bool DevServer::Start(std::string& err)
	{
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
			err = "WSAStartup failed";
			return false;
		}
		SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s == INVALID_SOCKET) {
			err = "socket() failed, WSA " + std::to_string(WSAGetLastError());
			return false;
		}
		const BOOL excl = TRUE;  // nobody else may share the port
		setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port = htons(static_cast<u_short>(_cfg.port));
		inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);  // loopback only, never 0.0.0.0
		if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(s, 4) != 0) {
			err = "cannot listen on 127.0.0.1:" + std::to_string(_cfg.port) + ", WSA " + std::to_string(WSAGetLastError()) + " (port in use?)";
			closesocket(s);
			return false;
		}
		int len = sizeof(a);
		getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
		_port = ntohs(a.sin_port);
		u_long nb = 1;
		ioctlsocket(s, FIONBIO, &nb);
		_listen = s;
		_t0 = Now();
		_stop = false;
		_thread = std::thread([this] { Run(); });
		return true;
	}

	void DevServer::Stop()
	{
		_stop = true;
		if (_thread.joinable()) _thread.join();
		if (_listen != ~std::uintptr_t{ 0 }) {
			closesocket(static_cast<SOCKET>(_listen));
			_listen = ~std::uintptr_t{ 0 };
		}
	}

	// ---- producers ---------------------------------------------------------------------------------------------------
	void DevServer::SetMenus(std::vector<MenuInfo> menus)
	{
		std::lock_guard lk(_m);
		_menus = std::move(menus);
	}

	void DevServer::SendTrace(std::string_view level, std::string_view source, std::string_view menu, std::string_view node, std::string_view text)
	{
		if (!_authed) return;
		_out.Push(Kind::kTrace, MakeTrace(Now() - _t0, level, source, menu, node, {}, text));
	}

	void DevServer::SendMenuState(std::string_view menu, bool open)
	{
		if (_authed) _out.Push(Kind::kControl, MakeMenuState(menu, open));
	}

	void DevServer::SendWatch(std::string_view menu, std::string_view graph, std::string_view scope, std::string_view name, const Scalar& value)
	{
		if (!_authed) return;
		_stream.Submit("w|" + std::string(menu) + "|" + std::string(graph) + "|" + std::string(scope) + "|" + std::string(name), MakeWatch(menu, graph, scope, name, value));
	}

	void DevServer::SendPin(std::string_view menu, std::string_view node, std::string_view pin, const Scalar& value)
	{
		if (!_authed) return;
		Scalar v = value;
		if (auto* s = std::get_if<std::string>(&v)) *s = CutPinText(std::move(*s));
		_stream.Submit("p|" + std::string(menu) + "|" + std::string(node) + "|" + std::string(pin), MakePin(menu, node, pin, v));
	}

	void DevServer::SendReloadAck(double id, bool ok, const std::vector<std::string>& reloaded, const std::vector<SkippedMenu>& skipped, std::int64_t ms, std::string_view error)
	{
		if (_authed) _out.Push(Kind::kControl, MakeReloadAck(id, ok, reloaded, skipped, ms, error));
	}

	void DevServer::SendError(std::string_view code, std::string_view message, std::optional<double> id)
	{
		if (_authed) _out.Push(Kind::kError, MakeError(code, message, id));
	}

	bool DevServer::WantsWatch(std::string_view menu) const
	{
		if (!_authed) return false;
		std::lock_guard lk(_m);
		const auto      it = _subs.find(Lower(menu));
		return it != _subs.end() && it->second.watch;
	}

	bool DevServer::WantsPins(std::string_view menu) const
	{
		if (!_authed) return false;
		std::lock_guard lk(_m);
		const auto      it = _subs.find(Lower(menu));
		return it != _subs.end() && it->second.pins;
	}

	// ---- server thread -----------------------------------------------------------------------------------------------
	void DevServer::Queue(Conn& c, const std::string& frame)
	{
		c.out += frame;
		if (c.out.size() > kOutHardCap) {
			Log(3, "dev client is not reading (more than 8 MiB unsent); dropping it");
			c.dead = true;
		}
	}

	void DevServer::Close(Conn& c, std::uint16_t code, const char* why)
	{
		Log(2, std::string("dev client closed: ") + why + " (" + std::to_string(code) + ")");
		Queue(c, ws::EncodeClose(code, why));
		c.closeAfterFlush = true;
	}

	void DevServer::Handle(Conn& c, const std::string& text)
	{
		const auto now = Now();
		auto       parsed = ParseEditorMessage(text);

		if (!c.authed) {
			const auto* h = parsed.msg ? std::get_if<Hello>(&*parsed.msg) : nullptr;
			if (!h) {
				Queue(c, ws::EncodeFrame(ws::kText, MakeError("bad-message", parsed.error.empty() ? "the first message must be a valid hello" : parsed.error)));
				Close(c, ws::kClosePolicy, "first message is not a valid hello");
				return;
			}
			HelloAckInfo ack;
			ack.runtimeVersion = _cfg.runtimeVersion;
			ack.runtimeVersionString = _cfg.runtimeVersionString;
			ack.gameVersion = _cfg.gameVersion;
			ack.protocolVersion = _cfg.protocolVersion;
			ack.specVersion = _cfg.specVersion;
			if (!ConstantTimeEqual(h->token, _cfg.token)) {
				_lockout.Fail(now);
				ack.ok = false;
				ack.error = "unauthorized";
				Queue(c, ws::EncodeFrame(ws::kText, MakeHelloAck(ack)));
				Log(3, "dev client sent a wrong token");
				Close(c, ws::kClosePolicy, "unauthorized");
				return;
			}
			if (h->protocolVersion != _cfg.protocolVersion) {
				ack.ok = false;
				ack.error = "protocol-mismatch";
				Queue(c, ws::EncodeFrame(ws::kText, MakeHelloAck(ack)));
				Close(c, ws::kClosePolicy, "protocol-mismatch");
				return;
			}
			{
				std::lock_guard lk(_m);
				ack.menus = _menus;
			}
			Queue(c, ws::EncodeFrame(ws::kText, MakeHelloAck(ack)));
			c.authed = true;
			c.lastPong = c.lastPing = now;
			_out.Clear();
			_stream.Clear();
			_authed = true;
			Log(2, "dev client connected: editor " + h->editorVersion + ", protocol " + std::to_string(h->protocolVersion) + ", spec " + std::to_string(h->specVersion) +
					   (h->specVersion != _cfg.specVersion ? " (spec version differs from the runtime's " + std::to_string(_cfg.specVersion) + ")" : ""));
			_host.OnClient(true);
			return;
		}

		if (!parsed.msg) {
			Queue(c, ws::EncodeFrame(ws::kText, MakeError("bad-message", parsed.error)));  // stay connected
			return;
		}
		if (const auto* s = std::get_if<Subscribe>(&*parsed.msg)) {
			std::string canonical;
			{
				std::lock_guard lk(_m);
				for (const auto& m : _menus) {
					if (Lower(m.name) == Lower(s->menu)) canonical = m.name;
				}
			}
			if (canonical.empty()) {
				Queue(c, ws::EncodeFrame(ws::kText, MakeError("unknown-menu", "no registered menu '" + s->menu + "'")));
				return;
			}
			bool watch, pins, watchWasOff;
			{
				std::lock_guard lk(_m);
				auto&           sub = _subs[Lower(canonical)];
				watchWasOff = !sub.watch;
				if (s->on) {
					sub.watch = s->watch;
					sub.pins = s->pins;
				} else if (!s->watch && !s->pins) {
					sub = {};  // unsubscribe with neither flag = everything
				} else {
					if (s->watch) sub.watch = false;
					if (s->pins) sub.pins = false;
				}
				watch = sub.watch;
				pins = sub.pins;
			}
			Log(1, "dev: " + std::string(s->on ? "subscribe" : "unsubscribe") + " " + canonical + " watch=" + (watch ? "1" : "0") + " pins=" + (pins ? "1" : "0"));
			_host.OnSubscribe(canonical, watch, pins, watch && watchWasOff);
		} else if (const auto* r = std::get_if<Reload>(&*parsed.msg)) {
			Log(2, "dev: reload request " + std::to_string(static_cast<long long>(r->id)) + (r->menu ? " for " + *r->menu : " for all menus"));
			_host.OnReload(r->id, r->menu);
		} else if (const auto* p = std::get_if<Ping>(&*parsed.msg)) {
			Queue(c, ws::EncodeFrame(ws::kText, MakePong(p->id)));
		} else if (std::holds_alternative<Pong>(*parsed.msg)) {
			c.lastPong = now;
		} else if (const auto* u = std::get_if<Unknown>(&*parsed.msg)) {
			Log(1, "dev: ignored message of unknown type '" + u->type.substr(0, 64) + "'");
		} else {
			Queue(c, ws::EncodeFrame(ws::kText, MakeError("bad-message", "hello was already sent")));
		}
	}

	void DevServer::Run()
	{
		const SOCKET lsock = static_cast<SOCKET>(_listen);
		char         buf[16384];
		while (!_stop) {
			fd_set rd, wr;
			FD_ZERO(&rd);
			FD_ZERO(&wr);
			FD_SET(lsock, &rd);
			for (auto& c : _conns) {
				FD_SET(c->s, &rd);
				if (!c->out.empty()) FD_SET(c->s, &wr);
			}
			timeval tv{ 0, 20'000 };
			const int n = select(0, &rd, &wr, nullptr, &tv);
			const auto now = Now();

			if (n > 0 && FD_ISSET(lsock, &rd)) {
				SOCKET ns = accept(lsock, nullptr, nullptr);
				if (ns != INVALID_SOCKET) {
					if (_lockout.Locked(now) || _conns.size() >= kMaxConns) {
						closesocket(ns);  // refused (S6 rule 4 lockout, or too many peers)
					} else {
						u_long nb = 1;
						ioctlsocket(ns, FIONBIO, &nb);
						const BOOL nd = TRUE;
						setsockopt(ns, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
						auto c = std::make_unique<Conn>(_cfg.maxMessageBytes);
						c->s = ns;
						c->accepted = c->lastPong = c->lastPing = now;
						_conns.push_back(std::move(c));
					}
				}
			}

			for (auto& cp : _conns) {
				Conn& c = *cp;
				if (c.dead) continue;
				if (n > 0 && FD_ISSET(c.s, &rd) && !c.closeAfterFlush) {
					const int got = recv(c.s, buf, sizeof(buf), 0);
					if (got == 0 || (got < 0 && WSAGetLastError() != WSAEWOULDBLOCK)) {
						c.dead = true;
						continue;
					}
					if (got > 0) {
						if (!c.upgraded) {
							c.in.append(buf, got);
							const auto u = ws::ParseUpgrade(c.in, _port);
							if (u.status == ws::UpgradeStatus::kOk) {
								c.upgraded = true;
								Queue(c, ws::UpgradeResponse(u.key));
								const std::string rest = c.in.substr(u.consumed);
								c.in.clear();
								// one editor at a time (S6 rule 1)
								bool busy = false;
								for (auto& o : _conns) busy = busy || (o.get() != &c && o->primary && !o->dead);
								if (busy) {
									Log(2, "second dev client refused: busy");
									Queue(c, ws::EncodeFrame(ws::kText, MakeError("busy", "another editor is connected")));
									Close(c, ws::kClosePolicy, "busy");
								} else {
									c.primary = true;
								}
								if (!rest.empty()) c.reader.Feed(rest.data(), rest.size());
							} else if (u.status != ws::UpgradeStatus::kNeedMore) {
								const int code = u.status == ws::UpgradeStatus::kForbidden ? 403 : u.status == ws::UpgradeStatus::kTooLarge ? 431 : 400;
								Log(2, "dev connection refused (" + std::to_string(code) + "): " + u.reason);
								Queue(c, ws::HttpError(code, code == 403 ? "Forbidden" : code == 431 ? "Request Header Fields Too Large" : "Bad Request"));
								c.closeAfterFlush = true;
							}
						} else {
							c.reader.Feed(buf, got);
						}
					}
				}
				// frames
				if (c.upgraded && !c.closeAfterFlush) {
					ws::Message m;
					for (;;) {
						const auto st = c.reader.Next(m);
						if (st == ws::Reader::Status::kNeedMore) break;
						if (st == ws::Reader::Status::kError) {
							Close(c, c.reader.CloseCode(), c.reader.ErrorText());
							break;
						}
						if (m.opcode == ws::kText) {
							if (!ws::IsValidUtf8(m.payload)) {
								Close(c, ws::kCloseBadData, "text is not UTF-8");
								break;
							}
							Handle(c, m.payload);
						} else if (m.opcode == ws::kBinary) {
							Close(c, ws::kCloseUnsupported, "binary frames are not supported");
							break;
						} else if (m.opcode == ws::kPing) {
							Queue(c, ws::EncodeFrame(ws::kPong, m.payload));
						} else if (m.opcode == ws::kPong) {
							c.lastPong = now;
						} else if (m.opcode == ws::kClose) {
							Queue(c, ws::EncodeFrame(ws::kClose, m.payload.substr(0, 2)));
							c.closeAfterFlush = true;
							break;
						}
						if (c.closeAfterFlush) break;
					}
				}
				// timers
				if (c.closeAfterFlush && !c.authed && now - c.accepted > _cfg.handshakeMs + 5000) c.dead = true;  // peer never reads our close
				if (!c.closeAfterFlush) {
					if (!c.authed && now - c.accepted > _cfg.handshakeMs) {
						if (c.upgraded) Close(c, ws::kClosePolicy, "no valid hello in time");
						else c.dead = true;
					} else if (c.authed) {
						if (now - c.lastPong > _cfg.pongTimeoutMs) {
							Close(c, ws::kCloseGoingAway, "no pong in time");
						} else if (now - c.lastPing >= _cfg.pingEveryMs) {
							c.lastPing = now;
							Queue(c, ws::EncodeFrame(ws::kText, MakePing(++c.pingId)));
						}
					}
				}
			}

			// stream limiter -> queue -> the authenticated client
			for (auto& [key, json] : _stream.Flush(now)) _out.Push(key[0] == 'p' ? Kind::kPin : Kind::kWatch, std::move(json));
			for (auto& cp : _conns) {
				Conn& c = *cp;
				if (!c.authed || c.dead || c.closeAfterFlush) continue;
				if (const auto dropped = _out.TakeDropped()) {
					Queue(c, ws::EncodeFrame(ws::kText, MakeTrace(now - _t0, "warn", "runtime", {}, {}, {}, "dev stream: " + std::to_string(dropped) + " message(s) dropped (queue full or client too slow)")));
				}
				std::string json;
				while (c.out.size() < kOutSoftCap && _out.Pop(json)) Queue(c, ws::EncodeFrame(ws::kText, json));
			}

			// write
			for (auto& cp : _conns) {
				Conn& c = *cp;
				if (c.dead || c.out.empty()) continue;
				const int sent = send(c.s, c.out.data(), static_cast<int>(std::min<std::size_t>(c.out.size(), 1 << 20)), 0);
				if (sent > 0) c.out.erase(0, static_cast<std::size_t>(sent));
				else if (sent < 0 && WSAGetLastError() != WSAEWOULDBLOCK) c.dead = true;
				if (c.out.empty() && c.closeAfterFlush) c.dead = true;
			}

			// reap
			for (auto it = _conns.begin(); it != _conns.end();) {
				Conn& c = **it;
				if (!c.dead) {
					++it;
					continue;
				}
				if (c.authed) {
					_authed = false;
					_out.Clear();
					_stream.Clear();
					{
						std::lock_guard lk(_m);
						_subs.clear();
					}
					Log(2, "dev client disconnected");
					_host.OnClient(false);
				}
				closesocket(c.s);
				it = _conns.erase(it);
			}
		}
		for (auto& c : _conns) closesocket(c->s);
		if (_authed) {
			_authed = false;
			_host.OnClient(false);
		}
		_conns.clear();
	}
}
