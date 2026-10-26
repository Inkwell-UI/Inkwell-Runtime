#include "WebSocket.h"

#include "Crypto.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace inkwell::ws
{
	namespace
	{
		constexpr std::size_t      kMaxHead = 8192;
		constexpr std::string_view kGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

		std::string Lower(std::string_view s)
		{
			std::string r(s);
			std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return r;
		}

		std::string_view Trim(std::string_view s)
		{
			while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
			while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
			return s;
		}

		bool HasToken(std::string_view list, std::string_view want)
		{
			std::size_t p = 0;
			while (p <= list.size()) {
				auto c = list.find(',', p);
				if (c == std::string_view::npos) c = list.size();
				if (Lower(Trim(list.substr(p, c - p))) == want) return true;
				p = c + 1;
			}
			return false;
		}

		std::string Header(std::uint8_t opcode, std::size_t len, bool fin, bool masked)
		{
			std::string h;
			h.push_back(static_cast<char>((fin ? 0x80 : 0) | (opcode & 0x0F)));
			const std::uint8_t m = masked ? 0x80 : 0;
			if (len < 126) {
				h.push_back(static_cast<char>(m | len));
			} else if (len <= 0xFFFF) {
				h.push_back(static_cast<char>(m | 126));
				h.push_back(static_cast<char>(len >> 8));
				h.push_back(static_cast<char>(len));
			} else {
				h.push_back(static_cast<char>(m | 127));
				for (int i = 7; i >= 0; --i) h.push_back(static_cast<char>((static_cast<std::uint64_t>(len) >> (i * 8)) & 0xFF));
			}
			return h;
		}
	}

	UpgradeResult ParseUpgrade(std::string_view buf, int port)
	{
		UpgradeResult r;
		const auto    end = buf.find("\r\n\r\n");
		if (end == std::string_view::npos || end > kMaxHead) {
			if (buf.size() > kMaxHead) {
				r.status = UpgradeStatus::kTooLarge;
				r.reason = "request head too large";
			}
			return r;
		}
		r.consumed = end + 4;
		const auto bad = [&](UpgradeStatus s, std::string why) {
			r.status = s;
			r.reason = std::move(why);
			return r;
		};

		const std::string_view head = buf.substr(0, end);
		const auto             lineEnd = head.find("\r\n");
		const auto             reqLine = head.substr(0, lineEnd);
		if (reqLine.size() < 14 || reqLine.substr(0, 4) != "GET " || reqLine.substr(reqLine.size() - 9) != " HTTP/1.1") return bad(UpgradeStatus::kBadRequest, "not a GET ... HTTP/1.1 request");

		std::map<std::string, std::string> h;
		std::size_t                        p = lineEnd == std::string_view::npos ? head.size() : lineEnd + 2;
		while (p < head.size()) {
			auto e = head.find("\r\n", p);
			if (e == std::string_view::npos) e = head.size();
			const auto line = head.substr(p, e - p);
			p = e + 2;
			const auto colon = line.find(':');
			if (colon == std::string_view::npos || colon == 0) return bad(UpgradeStatus::kBadRequest, "malformed header line");
			auto& slot = h[Lower(Trim(line.substr(0, colon)))];
			if (!slot.empty()) slot += ", ";
			slot += std::string(Trim(line.substr(colon + 1)));
		}

		// S6 rule 2: the security checks come first
		if (h.count("origin")) return bad(UpgradeStatus::kForbidden, "Origin header present (a browser page)");
		const std::string host = Lower(h.count("host") ? h["host"] : "");
		const std::string ps = std::to_string(port);
		if (host != "127.0.0.1:" + ps && host != "localhost:" + ps) return bad(UpgradeStatus::kForbidden, "Host header is not 127.0.0.1:<port> or localhost:<port>");

		if (!h.count("upgrade") || !HasToken(h["upgrade"], "websocket")) return bad(UpgradeStatus::kBadRequest, "no Upgrade: websocket");
		if (!h.count("connection") || !HasToken(h["connection"], "upgrade")) return bad(UpgradeStatus::kBadRequest, "no Connection: Upgrade");
		if (!h.count("sec-websocket-version") || h["sec-websocket-version"] != "13") return bad(UpgradeStatus::kBadRequest, "Sec-WebSocket-Version must be 13");
		const auto key = h.count("sec-websocket-key") ? h["sec-websocket-key"] : std::string();
		if (key.size() != 24 || key.substr(22) != "==") return bad(UpgradeStatus::kBadRequest, "bad Sec-WebSocket-Key");
		r.status = UpgradeStatus::kOk;
		r.key = key;
		return r;
	}

	std::string AcceptKey(std::string_view key)
	{
		const auto d = Sha1(std::string(key) + std::string(kGuid));
		return Base64(d.data(), d.size());
	}

	std::string UpgradeResponse(std::string_view key)
	{
		return "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + AcceptKey(key) + "\r\n\r\n";
	}

	std::string HttpError(int code, std::string_view reason)
	{
		const std::string body = std::string(reason) + "\n";
		return "HTTP/1.1 " + std::to_string(code) + " " + std::string(reason) + "\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
	}

	std::string EncodeFrame(std::uint8_t opcode, std::string_view payload, bool fin)
	{
		return Header(opcode, payload.size(), fin, false) + std::string(payload);
	}

	std::string EncodeMaskedFrame(std::uint8_t opcode, std::string_view payload, const std::uint8_t mask[4], bool fin)
	{
		std::string out = Header(opcode, payload.size(), fin, true);
		out.append(reinterpret_cast<const char*>(mask), 4);
		for (std::size_t i = 0; i < payload.size(); ++i) out.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ mask[i & 3]));
		return out;
	}

	std::string EncodeClose(std::uint16_t code, std::string_view reason)
	{
		std::string p;
		p.push_back(static_cast<char>(code >> 8));
		p.push_back(static_cast<char>(code & 0xFF));
		p.append(reason.substr(0, 120));  // control payloads are at most 125 bytes
		return EncodeFrame(kClose, p);
	}

	bool IsValidUtf8(std::string_view s)
	{
		std::size_t i = 0;
		const auto  n = s.size();
		const auto  b = [&](std::size_t k) { return static_cast<std::uint8_t>(s[k]); };
		while (i < n) {
			const std::uint8_t c = b(i);
			if (c < 0x80) { ++i; continue; }
			std::size_t   len;
			std::uint32_t cp;
			if (c >= 0xC2 && c <= 0xDF) { len = 2; cp = c & 0x1F; }
			else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
			else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
			else return false;
			if (i + len > n) return false;
			for (std::size_t k = 1; k < len; ++k) {
				if ((b(i + k) & 0xC0) != 0x80) return false;
				cp = (cp << 6) | (b(i + k) & 0x3F);
			}
			if ((len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))) return false;
			i += len;
		}
		return true;
	}

	Reader::Status Reader::Next(Message& out)
	{
		if (_failed) return Status::kError;
		for (;;) {
			const std::size_t avail = _buf.size() - _pos;
			if (avail < 2) break;
			const auto* p = reinterpret_cast<const std::uint8_t*>(_buf.data() + _pos);
			const bool  fin = p[0] & 0x80;
			if (p[0] & 0x70) return Fail(kCloseProtocol, "reserved bits set");
			const std::uint8_t op = p[0] & 0x0F;
			const bool         masked = p[1] & 0x80;
			if (masked != _masked) return Fail(kCloseProtocol, _masked ? "client frame is not masked" : "server frame is masked");
			if ((op > 2 && op < 8) || op > 10) return Fail(kCloseProtocol, "reserved opcode");
			std::uint64_t len = p[1] & 0x7F;
			std::size_t   head = 2;
			if (len == 126) {
				if (avail < 4) break;
				len = (std::uint64_t{ p[2] } << 8) | p[3];
				head = 4;
			} else if (len == 127) {
				if (avail < 10) break;
				len = 0;
				for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
				if (len >> 63) return Fail(kCloseProtocol, "length has the top bit set");
				head = 10;
			}
			const bool control = op >= 8;
			if (control && (!fin || len > 125)) return Fail(kCloseProtocol, "bad control frame");
			// size limit before anything is buffered: a header may claim a huge payload
			if (!control && (len > _max || _msg.size() + len > _max)) return Fail(kCloseTooBig, "message too big");
			const std::size_t maskLen = masked ? 4 : 0;
			if (avail < head + maskLen + len) break;

			std::string payload(_buf.data() + _pos + head + maskLen, static_cast<std::size_t>(len));
			if (masked) {
				const auto* m = p + head;
				for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ m[i & 3]);
			}
			_pos += head + maskLen + static_cast<std::size_t>(len);
			if (_pos > 65536 && _pos * 2 > _buf.size()) {
				_buf.erase(0, _pos);
				_pos = 0;
			}

			if (control) {
				if (op == kClose && payload.size() == 1) return Fail(kCloseProtocol, "bad close payload");
				out.opcode = op;
				out.payload = std::move(payload);
				return Status::kMessage;
			}
			if (op == kContinuation) {
				if (_msgOp == 0) return Fail(kCloseProtocol, "continuation without a start frame");
			} else {
				if (_msgOp != 0) return Fail(kCloseProtocol, "new data frame inside a fragmented message");
				_msgOp = op;
				_msg.clear();
			}
			_msg += payload;
			if (fin) {
				out.opcode = _msgOp;
				out.payload = std::move(_msg);
				_msg.clear();
				_msgOp = 0;
				return Status::kMessage;
			}
		}
		return Status::kNeedMore;
	}
}
