// Dev mode (S6) tests, all engine-free:
//  - unit tests: SHA-1/base64, RFC 6455 vectors, malformed frames, handshake checks, message codec (shapes of Inkwell-Spec runtimeProtocol.ts),
//    queue/limiter/lockout, INI upsert, menu scan, directory watcher;
//  - end to end: the real DevServer on a loopback port with a C++ WebSocket client (handshake, token, busy, limits, ping/pong, flood, reload flow).
// Usage: InkwellDevTests <test-assets dir>          run everything
//        InkwellDevTests --serve <port> <token>     keep a server with a fake host up (for scripts/dev-client.mjs)
#include "core/Crypto.h"
#include "core/DevProtocol.h"
#include "core/DevServer.h"
#include "core/DirWatcher.h"
#include "core/MenuScan.h"
#include "core/WebSocket.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>

using json = nlohmann::json;
using namespace inkwell;
namespace fs = std::filesystem;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                                    \
	do {                                                                               \
		++g_checks;                                                                    \
		if (!(cond)) {                                                                 \
			++g_failed;                                                                \
			std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n"; \
		}                                                                              \
	} while (0)

static std::string Bytes(std::initializer_list<int> b)
{
	std::string s;
	for (int x : b) s.push_back(static_cast<char>(x));
	return s;
}

static std::string Hex(const std::array<std::uint8_t, 20>& d) { return HexLower(d.data(), d.size()); }

// ---- unit tests --------------------------------------------------------------------------------------------------------
static void TestCrypto()
{
	CHECK(Hex(Sha1("abc")) == "a9993e364706816aba3e25717850c26c9cd0d89d");
	CHECK(Hex(Sha1("")) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
	CHECK(Hex(Sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
	CHECK(Hex(Sha1(std::string(1000000, 'a'))) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
	CHECK(ws::AcceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");  // RFC 6455 1.3
	const std::uint8_t b1[] = { 'M', 'a', 'n' }, b2[] = { 'M', 'a' }, b3[] = { 'M' };
	CHECK(Base64(b1, 3) == "TWFu" && Base64(b2, 2) == "TWE=" && Base64(b3, 1) == "TQ==" && Base64(b1, 0).empty());
	CHECK(ConstantTimeEqual("abc", "abc") && !ConstantTimeEqual("abc", "abd") && !ConstantTimeEqual("abc", "abcd") && !ConstantTimeEqual("", "a") && ConstantTimeEqual("", ""));
	const auto t1 = GenerateToken(), t2 = GenerateToken();
	CHECK(IsValidToken(t1) && IsValidToken(t2) && t1 != t2);
	CHECK(!IsValidToken("ABCDEF") && !IsValidToken(std::string(64, 'g')) && IsValidToken(std::string(64, 'a')));
	const auto m = MaskToken(t1);
	CHECK(m.find(t1.substr(4, 56)) == std::string::npos && m.substr(0, 4) == t1.substr(0, 4) && m.substr(m.size() - 4) == t1.substr(60));
}

static void TestFrames()
{
	const std::uint8_t mask[4] = { 0x37, 0xfa, 0x21, 0x3d };
	// RFC 6455 5.7 examples
	CHECK(ws::EncodeFrame(ws::kText, "Hello") == Bytes({ 0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f }));
	CHECK(ws::EncodeMaskedFrame(ws::kText, "Hello", mask) == Bytes({ 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 }));
	CHECK(ws::EncodeFrame(ws::kPing, "Hello") == Bytes({ 0x89, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f }));
	CHECK(ws::EncodeMaskedFrame(ws::kPong, "Hello", mask) == Bytes({ 0x8a, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 }));
	CHECK(ws::EncodeFrame(ws::kBinary, std::string(256, 'x')).substr(0, 4) == Bytes({ 0x82, 0x7E, 0x01, 0x00 }));
	CHECK(ws::EncodeFrame(ws::kBinary, std::string(65536, 'x')).substr(0, 10) == Bytes({ 0x82, 0x7F, 0, 0, 0, 0, 0, 1, 0, 0 }));
	CHECK(ws::EncodeClose(1000) == Bytes({ 0x88, 0x02, 0x03, 0xE8 }));

	ws::Message m;
	{  // masked "Hello" from a client
		ws::Reader r(1024, true);
		const auto f = Bytes({ 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 });
		r.Feed(f.data(), 3);
		CHECK(r.Next(m) == ws::Reader::Status::kNeedMore);
		r.Feed(f.data() + 3, f.size() - 3);
		CHECK(r.Next(m) == ws::Reader::Status::kMessage && m.opcode == ws::kText && m.payload == "Hello");
		CHECK(r.Next(m) == ws::Reader::Status::kNeedMore);
	}
	{  // unmasked "Hello" from a server (client side reader)
		ws::Reader r(1024, false);
		const auto f = ws::EncodeFrame(ws::kText, "Hello");
		r.Feed(f.data(), f.size());
		CHECK(r.Next(m) == ws::Reader::Status::kMessage && m.payload == "Hello");
	}
	{  // fragmented "Hel" + "lo" with a ping in between (RFC 5.4)
		ws::Reader r(1024, true);
		std::string f = ws::EncodeMaskedFrame(ws::kText, "Hel", mask, false) + ws::EncodeMaskedFrame(ws::kPing, "p", mask) + ws::EncodeMaskedFrame(ws::kContinuation, "lo", mask);
		r.Feed(f.data(), f.size());
		CHECK(r.Next(m) == ws::Reader::Status::kMessage && m.opcode == ws::kPing && m.payload == "p");
		CHECK(r.Next(m) == ws::Reader::Status::kMessage && m.opcode == ws::kText && m.payload == "Hello");
	}
	{  // two frames in one feed, 64-bit length form
		ws::Reader r(100000, true);
		std::string big(70000, 'z');
		std::string f = ws::EncodeMaskedFrame(ws::kText, big, mask) + ws::EncodeMaskedFrame(ws::kText, "x", mask);
		r.Feed(f.data(), f.size());
		CHECK(r.Next(m) == ws::Reader::Status::kMessage && m.payload == big);
		CHECK(r.Next(m) == ws::Reader::Status::kMessage && m.payload == "x");
	}
	const auto fails = [&](const std::string& bytes, std::uint16_t code, std::size_t max = 1024) {
		ws::Reader r(max, true);
		r.Feed(bytes.data(), bytes.size());
		ws::Message mm;
		const bool  ok = r.Next(mm) == ws::Reader::Status::kError && r.CloseCode() == code && r.Next(mm) == ws::Reader::Status::kError;  // stays failed
		return ok;
	};
	CHECK(fails(Bytes({ 0x81, 0x05, 'H', 'e', 'l', 'l', 'o' }), ws::kCloseProtocol));               // client frame not masked
	CHECK(fails(Bytes({ 0xC1, 0x80, 0, 0, 0, 0 }), ws::kCloseProtocol));                            // RSV1
	CHECK(fails(Bytes({ 0x83, 0x80, 0, 0, 0, 0 }), ws::kCloseProtocol));                            // reserved opcode 3
	CHECK(fails(Bytes({ 0x8B, 0x80, 0, 0, 0, 0 }), ws::kCloseProtocol));                            // reserved opcode 11
	CHECK(fails(Bytes({ 0x09, 0x80, 0, 0, 0, 0 }), ws::kCloseProtocol));                            // fragmented ping
	CHECK(fails(Bytes({ 0x89, 0xFE, 0x00, 0x7E }), ws::kCloseProtocol));                            // ping longer than 125
	CHECK(fails(Bytes({ 0x80, 0x80, 0, 0, 0, 0 }), ws::kCloseProtocol));                            // continuation without a start
	CHECK(fails(Bytes({ 0x01, 0x80, 0, 0, 0, 0, 0x01, 0x80, 0, 0, 0, 0 }), ws::kCloseProtocol));  // new data frame inside a fragmented one
	CHECK(fails(Bytes({ 0x81, 0xFF, 0x80, 0, 0, 0, 0, 0, 0, 0 }), ws::kCloseProtocol));            // length with the top bit
	CHECK(fails(Bytes({ 0x88, 0x81, 0, 0, 0, 0, 0 }), ws::kCloseProtocol));                         // close with 1-byte payload
	CHECK(fails(Bytes({ 0x81, 0xFF, 0, 0, 1, 0, 0, 0, 0, 0 }), ws::kCloseTooBig));                  // header claims 1 TiB, nothing buffered
	CHECK(fails(Bytes({ 0x81, 0xFE, 0x08, 0x00 }), ws::kCloseTooBig));                              // 2048 > max 1024, rejected on the header alone
	{  // a message that is only too big in total
		ws::Reader r(1024, true);
		const std::uint8_t mk[4] = { 1, 2, 3, 4 };
		std::string f = ws::EncodeMaskedFrame(ws::kText, std::string(600, 'a'), mk, false) + ws::EncodeMaskedFrame(ws::kContinuation, std::string(600, 'a'), mk);
		r.Feed(f.data(), f.size());
		CHECK(r.Next(m) == ws::Reader::Status::kError && r.CloseCode() == ws::kCloseTooBig);
	}
	// fuzz: random bytes never crash or loop, and byte-at-a-time feeding agrees with one feed
	std::mt19937 rng(12345);
	for (int i = 0; i < 3000; ++i) {
		std::string junk(rng() % 80, '\0');
		for (auto& c : junk) c = static_cast<char>(rng());
		if (!junk.empty() && i % 2) junk[0] = static_cast<char>(0x80 | (rng() & 0x0F));
		ws::Reader a(256, true), b(256, true);
		a.Feed(junk.data(), junk.size());
		int na = 0, nb = 0;
		ws::Message x;
		for (int k = 0; k < 100 && a.Next(x) == ws::Reader::Status::kMessage; ++k) ++na;
		for (char c : junk) {
			b.Feed(&c, 1);
			while (b.Next(x) == ws::Reader::Status::kMessage) ++nb;
		}
		CHECK(na == nb);
		(void)ws::ParseUpgrade(junk, 8788);
		(void)ws::IsValidUtf8(junk);
		(void)dev::ParseEditorMessage(junk);
	}
	CHECK(ws::IsValidUtf8("plain \xC3\xA4 \xE2\x82\xAC \xF0\x9F\x98\x80") && !ws::IsValidUtf8("\xC0\xAF") && !ws::IsValidUtf8("\xED\xA0\x80") && !ws::IsValidUtf8("\xE2\x82") &&
		  !ws::IsValidUtf8("\xF4\x90\x80\x80") && !ws::IsValidUtf8("\x80"));
}

static std::string Req(const std::string& host, const std::string& extra = "", const std::string& key = "dGhlIHNhbXBsZSBub25jZQ==")
{
	return "GET / HTTP/1.1\r\nHost: " + host + "\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\nSec-WebSocket-Key: " + key + "\r\nSec-WebSocket-Version: 13\r\n" + extra + "\r\n";
}

static void TestUpgrade()
{
	using S = ws::UpgradeStatus;
	auto r = ws::ParseUpgrade(Req("127.0.0.1:8788") + "tail", 8788);
	CHECK(r.status == S::kOk && r.key == "dGhlIHNhbXBsZSBub25jZQ==" && r.consumed == Req("127.0.0.1:8788").size());
	CHECK(ws::ParseUpgrade(Req("LocalHost:8788"), 8788).status == S::kOk);
	CHECK(ws::ParseUpgrade(Req("127.0.0.1:8788", "Origin: http://evil.example\r\n"), 8788).status == S::kForbidden);  // a web page
	CHECK(ws::ParseUpgrade(Req("127.0.0.1:8788", "origin: null\r\n"), 8788).status == S::kForbidden);
	CHECK(ws::ParseUpgrade(Req("evil.example:8788"), 8788).status == S::kForbidden);  // DNS rebinding
	CHECK(ws::ParseUpgrade(Req("127.0.0.1:9999"), 8788).status == S::kForbidden);
	CHECK(ws::ParseUpgrade(Req("127.0.0.1"), 8788).status == S::kForbidden);
	CHECK(ws::ParseUpgrade(Req("127.0.0.1:8788", "", "short"), 8788).status == S::kBadRequest);
	CHECK(ws::ParseUpgrade("GET / HTTP/1.1\r\nHost: 127.0.0.1:8788\r\n\r\n", 8788).status == S::kBadRequest);
	CHECK(ws::ParseUpgrade("POST / HTTP/1.1\r\nHost: 127.0.0.1:8788\r\n\r\n", 8788).status == S::kBadRequest);
	CHECK(ws::ParseUpgrade("GET / HTTP/1.1\r\nHost: 127.0.0.1", 8788).status == S::kNeedMore);
	CHECK(ws::ParseUpgrade(std::string(9000, 'a'), 8788).status == S::kTooLarge);
	CHECK(ws::UpgradeResponse("dGhlIHNhbXBsZSBub25jZQ==").find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);
}

static void TestCodec()
{
	using namespace dev;
	// shapes of Inkwell-Spec/src/runtimeProtocol.ts
	auto p = ParseEditorMessage(R"({"type":"hello","token":")" + std::string(64, 'a') + R"(","editorVersion":"0.3.0","protocolVersion":1,"specVersion":6,"future":true})");
	CHECK(p.msg && std::get<Hello>(*p.msg).protocolVersion == 1 && std::get<Hello>(*p.msg).token.size() == 64);
	p = ParseEditorMessage(R"({"type":"subscribe","menu":"M"})");
	CHECK(p.msg && std::get<Subscribe>(*p.msg).on && !std::get<Subscribe>(*p.msg).watch && !std::get<Subscribe>(*p.msg).pins);
	p = ParseEditorMessage(R"({"type":"unsubscribe","menu":"M","watch":true,"pins":false})");
	CHECK(p.msg && !std::get<Subscribe>(*p.msg).on && std::get<Subscribe>(*p.msg).watch);
	p = ParseEditorMessage(R"({"type":"reload","id":7})");
	CHECK(p.msg && std::get<Reload>(*p.msg).id == 7 && !std::get<Reload>(*p.msg).menu);
	p = ParseEditorMessage(R"({"type":"reload","id":7,"menu":"M"})");
	CHECK(p.msg && *std::get<Reload>(*p.msg).menu == "M");
	p = ParseEditorMessage(R"({"type":"ping"})");
	CHECK(p.msg && std::holds_alternative<Ping>(*p.msg) && !std::get<Ping>(*p.msg).id);
	p = ParseEditorMessage(R"({"type":"pong","id":3})");
	CHECK(p.msg && *std::get<Pong>(*p.msg).id == 3);
	p = ParseEditorMessage(R"({"type":"somethingNew","x":1})");
	CHECK(p.msg && std::holds_alternative<Unknown>(*p.msg) && p.error.empty());
	for (const char* bad : { "", "x", "[]", "null", "{}", R"({"type":5})", R"({"type":"hello"})", R"({"type":"hello","token":1,"editorVersion":"","protocolVersion":1,"specVersion":6})",
			 R"({"type":"hello","token":"a","editorVersion":"","protocolVersion":"1","specVersion":6})", R"({"type":"subscribe"})", R"({"type":"subscribe","menu":"M","watch":"yes"})",
			 R"({"type":"reload"})", R"({"type":"reload","id":"1"})", R"({"type":"reload","id":1,"menu":3})", R"({"type":"ping","id":"x"})" }) {
		p = ParseEditorMessage(bad);
		CHECK(!p.msg && !p.error.empty());
	}
	CHECK(!ParseEditorMessage(std::string(100000, '[')).msg);  // deep nesting does not blow the stack
	CHECK(!ParseEditorMessage("{\"type\":\"hello\"\0}").msg);

	HelloAckInfo a;
	a.runtimeVersion = 196608;
	a.runtimeVersionString = "0.3.0";
	a.gameVersion = "1.5.97.0";
	a.menus = { { "A", "Interface/A.swf", true } };
	auto j = json::parse(MakeHelloAck(a));
	CHECK(j["type"] == "helloAck" && j["ok"] == true && j["runtimeVersion"] == 196608 && j["menus"][0]["name"] == "A" && j["menus"][0]["open"] == true && !j.contains("error"));
	a.ok = false;
	a.error = "unauthorized";
	CHECK(json::parse(MakeHelloAck(a))["error"] == "unauthorized");
	j = json::parse(MakeTrace(12, "warn", "as2", "M", "n1", "", "text"));
	CHECK(j["type"] == "trace" && j["ts"] == 12 && j["level"] == "warn" && j["source"] == "as2" && j["menu"] == "M" && j["node"] == "n1" && !j.contains("graph") && j["text"] == "text");
	j = json::parse(MakeTrace(1, "info", "runtime", "", "", "", "bad utf8 \xFF\xFE"));  // must not throw
	CHECK(j["type"] == "trace" && !j.contains("menu"));
	j = json::parse(MakeWatch("M", "g", "saved", "n", Scalar{ 2.5 }));
	CHECK(j["type"] == "watch" && j["scope"] == "saved" && j["value"] == 2.5);
	CHECK(json::parse(MakeWatch("M", "g", "menu", "n", Scalar{ std::monostate{} }))["value"].is_null());
	CHECK(json::parse(MakePin("M", "n", "out", Scalar{ true }))["value"] == true);
	CHECK(json::parse(MakePin("M", "n", "out", Scalar{ std::string("s") }))["value"] == "s");
	j = json::parse(MakeReloadAck(5, true, { "A" }, { { "B", "unchanged" } }, 42, ""));
	CHECK(j["id"] == 5 && j["id"].is_number_integer() && j["ok"] == true && j["reloaded"][0] == "A" && j["skipped"][0]["reason"] == "unchanged" && j["ms"] == 42 && !j.contains("error"));
	j = json::parse(MakeError("busy", "x", 3));
	CHECK(j["type"] == "error" && j["code"] == "busy" && j["id"] == 3 && !json::parse(MakeError("busy", "x")).contains("id"));
	CHECK(json::parse(MakePing(2))["id"] == 2 && !json::parse(MakePong(std::nullopt)).contains("id"));
	CHECK(json::parse(MakeMenuState("M", false))["open"] == false);

	CHECK(CutPinText(std::string(600, 'a')).size() == 512 && CutPinText("short") == "short");
	std::string u;
	for (int i = 0; i < 600; ++i) u += "\xC3\xA4";
	const auto cut = CutPinText(u);
	CHECK(cut.size() == 1024 && ws::IsValidUtf8(cut));
}

static void TestQueueAndLimits()
{
	using namespace dev;
	OutQueue q(4);
	q.Push(Kind::kPin, "p1");
	q.Push(Kind::kWatch, "w1");
	q.Push(Kind::kTrace, "t1");
	q.Push(Kind::kError, "e1");
	q.Push(Kind::kTrace, "t2");  // full: drops the oldest pin
	std::string s;
	CHECK(q.Size() == 4 && q.TakeDropped() == 1 && q.TakeDropped() == 0);
	CHECK(q.Pop(s) && s == "w1");
	q.Push(Kind::kPin, "p2");
	q.Push(Kind::kPin, "p3");  // full again: a new pin is not worth more than what is queued
	CHECK(q.Size() == 4 && q.TakeDropped() == 1);
	OutQueue f(3);
	for (int i = 0; i < 3; ++i) f.Push(Kind::kError, "e" + std::to_string(i));
	f.Push(Kind::kError, "e3");  // errors drop only each other (oldest)
	CHECK(f.Pop(s) && s == "e1" && f.TakeDropped() == 1);
	OutQueue big;
	for (int i = 0; i < 5000; ++i) big.Push(Kind::kTrace, "x");
	CHECK(big.Size() == kQueueSize && big.TakeDropped() == 4000);

	StreamLimiter lim;
	lim.Submit("p|M|n|a", "v1");
	lim.Submit("p|M|n|a", "v2");  // coalesced
	auto out = lim.Flush(1000);
	CHECK(out.size() == 1 && out[0].second == "v2");
	lim.Submit("p|M|n|a", "v3");
	CHECK(lim.Flush(1010).empty());                                  // 20/s per pin: 50 ms apart
	out = lim.Flush(1060);
	CHECK(out.size() == 1 && out[0].second == "v3");
	StreamLimiter total(20, 200);
	for (int i = 0; i < 500; ++i) total.Submit("k" + std::to_string(i), "v");
	CHECK(total.Flush(5000).size() == 200);                          // 200 per second in total
	CHECK(total.Flush(5100).empty());
	CHECK(total.Flush(6000).size() == 200);
	StreamLimiter tiny(20, 200, 2);
	CHECK(tiny.Submit("a", "1") && tiny.Submit("b", "1") && !tiny.Submit("c", "1") && tiny.Submit("a", "2"));

	Lockout l;
	for (int i = 0; i < 4; ++i) l.Fail(1000 + i);
	CHECK(!l.Locked(1010));
	l.Fail(1010);
	CHECK(l.Locked(1011) && l.Locked(60'000) && !l.Locked(61'011));
	Lockout slow;  // five failures spread over more than a minute do not lock
	for (int i = 0; i < 5; ++i) slow.Fail(i * 20'000);
	CHECK(!slow.Locked(100'000));

	Debouncer d(300);
	d.Add("a", 100);
	CHECK(d.Due(200).empty());
	d.Add("b", 350);
	CHECK(d.Due(500).empty());
	const auto due = d.Due(700);
	CHECK(due.size() == 2 && d.Due(2000).empty());
}

static void TestIniUpsert()
{
	CHECK(UpsertIniValue("", "Dev", "Token", "abc") == "[Dev]\nToken=abc\n");
	CHECK(UpsertIniValue("[Runtime]\nLogLevel=debug\n", "Dev", "Token", "abc") == "[Runtime]\nLogLevel=debug\n\n[Dev]\nToken=abc\n");
	CHECK(UpsertIniValue("[Dev]\r\nDevMode=1\r\nToken=\r\n[Other]\r\nx=1\r\n", "dev", "Token", "abc") == "[Dev]\r\nDevMode=1\r\nToken=abc\r\n[Other]\r\nx=1\r\n");
	CHECK(UpsertIniValue("[Dev]\nDevMode=1\n\n[Other]\nx=1\n", "Dev", "Token", "abc") == "[Dev]\nDevMode=1\nToken=abc\n\n[Other]\nx=1\n");
	CHECK(UpsertIniValue("[Dev]\n; Token=old\nDevMode=1", "Dev", "Token", "abc") == "[Dev]\n; Token=old\nDevMode=1\nToken=abc\n");
	const auto both = UpsertIniValue("[Dev]\nDevMode=1\n", "Dev", "Token", std::string(64, 'b'));
	const auto parsed = ParseIni(both, IniKind::kRuntime);
	CHECK(parsed.devMode == true && parsed.token == std::string(64, 'b'));
}

static void TestScan(const fs::path& assets)
{
	auto r = ScanMenus(assets);  // test-assets/menus
	CHECK(!r.dirMissing && r.menus.size() == 5 && r.failedFiles.empty());
	CHECK(ScanMenus(assets / "nope").dirMissing);
	const fs::path tmp = fs::temp_directory_path() / "inkwell-scan-test";
	fs::remove_all(tmp);
	fs::create_directories(tmp / "menus");
	fs::copy_file(assets / "menus-bad" / "bad_01_syntax.json", tmp / "menus" / "bad.json");
	fs::copy_file(assets / "menus" / "InkwellTest_A.json", tmp / "menus" / "a.json");
	r = ScanMenus(tmp);
	CHECK(r.menus.size() == 1 && r.failedFiles.size() == 1 && r.failedFiles.count(LowerCopy((tmp / "menus" / "bad.json").string())) == 1);
	fs::copy_file(assets / "menus" / "InkwellTest_A.json", tmp / "menus" / "zz_dup.json");
	r = ScanMenus(tmp);
	CHECK(r.menus.size() == 1 && r.failedFiles.size() == 2);  // duplicate name rejected
	const auto s1 = StampOf(tmp / "menus" / "a.json");
	CHECK(s1.exists && s1.size > 0 && !StampOf(tmp / "missing").exists);
	fs::remove_all(tmp);
}

static void TestWatcher()
{
	const fs::path tmp = fs::temp_directory_path() / "inkwell-watch-test";
	fs::remove_all(tmp);
	fs::create_directories(tmp / "sub");
	DirWatcher                w;
	std::mutex                m;
	std::vector<std::set<std::string>> calls;
	std::vector<std::string>  skipped;
	CHECK(w.Start({ { tmp, true }, { tmp / "missing", false } }, 300, [&](const std::set<std::string>& s) { std::lock_guard lk(m); calls.push_back(s); }, skipped));
	CHECK(skipped.size() == 1);
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	for (int i = 0; i < 3; ++i) {  // a burst of writes is one callback
		std::ofstream(tmp / "sub" / "a.swf", std::ios::binary | std::ios::app) << "x";
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	for (int i = 0; i < 60; ++i) {
		{
			std::lock_guard lk(m);
			if (!calls.empty()) break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	{
		std::lock_guard lk(m);
		CHECK(calls.size() == 1);
		bool found = false;
		for (const auto& s : calls.empty() ? std::set<std::string>{} : calls[0]) found = found || s.find("a.swf") != std::string::npos;
		CHECK(found);
	}
	w.Stop();
	fs::remove_all(tmp);
}

// ---- end to end -----------------------------------------------------------------------------------------------------------
struct FakeHost : dev::DevHost
{
	std::mutex                         m;
	std::vector<std::string>           logs;
	std::atomic<int>                   clients{ 0 }, connects{ 0 };
	struct Sub { std::string menu; bool watch, pins, wasOff; };
	std::vector<Sub>                   subs;
	std::vector<std::pair<double, std::optional<std::string>>> reloads;
	bool                               verbose{ false };

	void Log(int level, const std::string& t) override
	{
		std::lock_guard lk(m);
		logs.push_back(t);
		if (verbose) std::cout << "[server " << level << "] " << t << std::endl;
	}
	void OnClient(bool c) override
	{
		clients += c ? 1 : -1;
		if (c) ++connects;
	}
	void OnSubscribe(const std::string& menu, bool w, bool p, bool off) override
	{
		std::lock_guard lk(m);
		subs.push_back({ menu, w, p, off });
	}
	void OnReload(double id, const std::optional<std::string>& menu) override
	{
		std::lock_guard lk(m);
		reloads.emplace_back(id, menu);
	}
};

struct Client
{
	SOCKET      s{ INVALID_SOCKET };
	ws::Reader  reader{ 4 * 1024 * 1024, false };
	bool        closed{ false };
	std::string head;

	~Client() { Close(); }
	void Close()
	{
		if (s != INVALID_SOCKET) closesocket(s);
		s = INVALID_SOCKET;
	}
	bool Connect(int port)
	{
		s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port = htons(static_cast<u_short>(port));
		inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
		return connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
	}
	void SendRaw(const std::string& b) { send(s, b.data(), static_cast<int>(b.size()), 0); }
	// reads until `n` bytes are available or timeout; appends into `into`. False on EOF/timeout with nothing
	bool ReadSome(std::string& into, int timeoutMs)
	{
		fd_set rd;
		FD_ZERO(&rd);
		FD_SET(s, &rd);
		timeval tv{ timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
		if (select(0, &rd, nullptr, nullptr, &tv) <= 0) return false;
		char      buf[65536];
		const int n = recv(s, buf, sizeof(buf), 0);
		if (n <= 0) {
			closed = true;
			return false;
		}
		into.append(buf, n);
		return true;
	}
	/// Sends an upgrade request and returns the HTTP head of the answer ("" if the server just closed).
	std::string Upgrade(int, const std::string& request)
	{
		SendRaw(request);
		std::string acc;
		for (int i = 0; i < 100; ++i) {
			const auto e = acc.find("\r\n\r\n");
			if (e != std::string::npos) {
				head = acc.substr(0, e + 4);
				const std::string rest = acc.substr(e + 4);
				// a 4xx answer has a body of Content-Length bytes; for 101 the rest is frames
				if (head.find(" 101 ") != std::string::npos) reader.Feed(rest.data(), rest.size());
				return head;
			}
			if (!ReadSome(acc, 200) && closed) break;
		}
		return acc;
	}
	void SendJson(const json& j)
	{
		static const std::uint8_t mask[4] = { 9, 8, 7, 6 };
		SendRaw(ws::EncodeMaskedFrame(ws::kText, j.dump(), mask));
	}
	void SendFrame(std::uint8_t op, const std::string& payload)
	{
		static const std::uint8_t mask[4] = { 1, 2, 3, 4 };
		SendRaw(ws::EncodeMaskedFrame(op, payload, mask));
	}
	/// Next message (any opcode); false on timeout. After EOF `closed` is true.
	bool Next(ws::Message& m, int timeoutMs = 3000)
	{
		const auto t0 = std::chrono::steady_clock::now();
		for (;;) {
			const auto st = reader.Next(m);
			if (st == ws::Reader::Status::kMessage) return true;
			if (st == ws::Reader::Status::kError) return false;
			const int left = timeoutMs - static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
			if (left <= 0 || closed) return false;
			std::string more;
			if (ReadSome(more, left)) reader.Feed(more.data(), more.size());
		}
	}
	/// Next text message as JSON (skips server pings by answering them).
	std::optional<json> NextJson(int timeoutMs = 3000)
	{
		ws::Message m;
		while (Next(m, timeoutMs)) {
			if (m.opcode == ws::kText) return json::parse(m.payload, nullptr, false);
			if (m.opcode == ws::kPing) SendFrame(ws::kPong, m.payload);
		}
		return std::nullopt;
	}
	/// Waits for a message of a type; others are skipped. nullopt on timeout.
	std::optional<json> WaitType(const std::string& type, int timeoutMs = 3000)
	{
		const auto t0 = std::chrono::steady_clock::now();
		for (;;) {
			const int left = timeoutMs - static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
			if (left <= 0) return std::nullopt;
			auto j = NextJson(left);
			if (!j) return std::nullopt;
			if (j->is_object() && j->value("type", "") == type) return j;
		}
	}
	/// Expects the server to close: returns the close code (0 = closed without a close frame, -1 = still open after the timeout).
	int WaitClose(int timeoutMs = 3000)
	{
		ws::Message m;
		for (;;) {
			if (!Next(m, timeoutMs)) return closed ? 0 : -1;
			if (m.opcode == ws::kClose) return m.payload.size() >= 2 ? (static_cast<std::uint8_t>(m.payload[0]) << 8) | static_cast<std::uint8_t>(m.payload[1]) : 1005;
		}
	}
};

static std::string GoodReq(int port, const std::string& extra = "") { return Req("127.0.0.1:" + std::to_string(port), extra); }

static json Hello(const std::string& token, int protocol = 1) { return { { "type", "hello" }, { "token", token }, { "editorVersion", "test" }, { "protocolVersion", protocol }, { "specVersion", 6 } }; }

static dev::ServerConfig Cfg(const std::string& token)
{
	dev::ServerConfig c;
	c.port = 0;
	c.token = token;
	c.runtimeVersion = 196608;
	c.runtimeVersionString = "0.3.0";
	c.gameVersion = "1.6.640.0";
	return c;
}

static bool Login(Client& c, int port, const std::string& token)
{
	if (!c.Connect(port) || c.Upgrade(port, GoodReq(port)).find(" 101 ") == std::string::npos) return false;
	c.SendJson(Hello(token));
	const auto a = c.WaitType("helloAck");
	return a && (*a)["ok"] == true;
}

static void TestServer()
{
	const std::string token = GenerateToken();
	FakeHost          host;
	dev::DevServer    srv(host, Cfg(token));
	std::string       err;
	CHECK(srv.Start(err));
	const int port = srv.Port();
	CHECK(port > 1023);
	srv.SetMenus({ { "InkwellTest_A", "Interface/Inkwell/T/A.swf", false }, { "InkwellTest_B", "Interface/Inkwell/T/B.swf", true } });

	{  // upgrade checks: 403 for Origin and Host, 400 for junk; the server is still fine afterwards
		Client c;
		CHECK(c.Connect(port));
		CHECK(c.Upgrade(port, GoodReq(port, "Origin: http://localhost:8788\r\n")).find(" 403 ") != std::string::npos);
		Client d;
		CHECK(d.Connect(port));
		CHECK(d.Upgrade(port, Req("evil.example:" + std::to_string(port))).find(" 403 ") != std::string::npos);
		Client e;
		CHECK(e.Connect(port));
		e.SendRaw("hello there\r\n\r\n");
		std::string acc;
		e.ReadSome(acc, 1000);
		CHECK(acc.find(" 400 ") != std::string::npos);
		Client f;
		CHECK(f.Connect(port));
		f.SendRaw(std::string(9000, 'A'));  // no header end, too large
		acc.clear();
		f.ReadSome(acc, 1000);
		CHECK(acc.find(" 431 ") != std::string::npos);
	}
	{  // wrong token, then protocol mismatch, then a non-hello first message
		Client c;
		CHECK(c.Connect(port) && c.Upgrade(port, GoodReq(port)).find(" 101 ") != std::string::npos);
		c.SendJson(Hello(std::string(64, '0')));
		auto a = c.WaitType("helloAck");
		CHECK(a && (*a)["ok"] == false && (*a)["error"] == "unauthorized" && (*a)["menus"].empty());
		CHECK(c.WaitClose() == 1008);
		Client d;
		CHECK(d.Connect(port) && d.Upgrade(port, GoodReq(port)).find(" 101 ") != std::string::npos);
		d.SendJson(Hello(token, 2));
		a = d.WaitType("helloAck");
		CHECK(a && (*a)["ok"] == false && (*a)["error"] == "protocol-mismatch");
		CHECK(d.WaitClose() == 1008);
		Client e;
		CHECK(e.Connect(port) && e.Upgrade(port, GoodReq(port)).find(" 101 ") != std::string::npos);
		e.SendJson({ { "type", "subscribe" }, { "menu", "InkwellTest_A" } });
		CHECK(e.WaitType("error") && e.WaitClose() == 1008);
		Client g;
		CHECK(g.Connect(port) && g.Upgrade(port, GoodReq(port)).find(" 101 ") != std::string::npos);
		g.SendFrame(ws::kText, "not json at all");
		const auto er = g.WaitType("error");
		CHECK(er && (*er)["code"] == "bad-message" && g.WaitClose() == 1008);
		CHECK(host.clients == 0);
	}

	Client ed;
	{  // good login
		CHECK(ed.Connect(port) && ed.Upgrade(port, GoodReq(port)).find(" 101 ") != std::string::npos);
		ed.SendJson(Hello(token));
		const auto a = ed.WaitType("helloAck");
		CHECK(a && (*a)["ok"] == true && (*a)["runtimeVersion"] == 196608 && (*a)["runtimeVersionString"] == "0.3.0" && (*a)["protocolVersion"] == 1 && (*a)["gameVersion"] == "1.6.640.0");
		CHECK(a && (*a)["menus"].size() == 2 && (*a)["menus"][1]["open"] == true);
		CHECK(host.clients == 1 && srv.Connected());
	}
	{  // a second editor is refused with error busy and closed; the first one keeps working
		Client c2;
		CHECK(c2.Connect(port) && c2.Upgrade(port, GoodReq(port)).find(" 101 ") != std::string::npos);
		const auto e = c2.WaitType("error");
		CHECK(e && (*e)["code"] == "busy");
		CHECK(c2.WaitClose() == 1008);
		ed.SendJson({ { "type", "ping" }, { "id", 11 } });
		const auto pg = ed.WaitType("pong");
		CHECK(pg && (*pg)["id"] == 11);
		CHECK(host.clients == 1);
	}
	{  // malformed and unknown messages after hello: error bad-message, connection stays; unknown type is ignored
		ed.SendFrame(ws::kText, "{broken");
		auto e = ed.WaitType("error");
		CHECK(e && (*e)["code"] == "bad-message");
		ed.SendJson({ { "type", "reload" } });
		e = ed.WaitType("error");
		CHECK(e && (*e)["code"] == "bad-message");
		ed.SendJson({ { "type", "futureThing" }, { "x", 1 } });
		ed.SendJson(Hello(token));  // a second hello
		e = ed.WaitType("error");
		CHECK(e && (*e)["code"] == "bad-message");
		ed.SendFrame(ws::kPing, "abc");  // protocol-level ping gets a pong frame with the same payload
		ws::Message m;
		bool        gotPong = false;
		for (int i = 0; i < 5 && ed.Next(m, 1000); ++i) gotPong = gotPong || (m.opcode == ws::kPong && m.payload == "abc");
		CHECK(gotPong);
		ed.SendJson({ { "type", "ping" } });
		CHECK(ed.WaitType("pong"));
	}
	{  // subscribe / unsubscribe
		ed.SendJson({ { "type", "subscribe" }, { "menu", "NoSuchMenu" }, { "watch", true } });
		const auto e = ed.WaitType("error");
		CHECK(e && (*e)["code"] == "unknown-menu");
		CHECK(!srv.WantsPins("InkwellTest_A"));
		ed.SendJson({ { "type", "subscribe" }, { "menu", "inkwelltest_a" }, { "watch", true }, { "pins", true } });
		for (int i = 0; i < 50 && !srv.WantsPins("InkwellTest_A"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
		CHECK(srv.WantsPins("InkwellTest_A") && srv.WantsWatch("INKWELLTEST_A") && !srv.WantsPins("InkwellTest_B"));
		{
			std::lock_guard lk(host.m);
			CHECK(host.subs.size() == 1 && host.subs[0].menu == "InkwellTest_A" && host.subs[0].watch && host.subs[0].pins && host.subs[0].wasOff);
		}
		ed.SendJson({ { "type", "unsubscribe" }, { "menu", "InkwellTest_A" }, { "pins", true } });
		for (int i = 0; i < 50 && srv.WantsPins("InkwellTest_A"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
		CHECK(!srv.WantsPins("InkwellTest_A") && srv.WantsWatch("InkwellTest_A"));
		ed.SendJson({ { "type", "subscribe" }, { "menu", "InkwellTest_A" }, { "pins", true } });  // replaces: watch off, pins on
		for (int i = 0; i < 50 && !srv.WantsPins("InkwellTest_A"); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
		CHECK(srv.WantsPins("InkwellTest_A") && !srv.WantsWatch("InkwellTest_A"));
	}
	{  // streaming to the editor: trace in order, menuState, watch, coalesced pins, reload flow
		srv.SendTrace("info", "as2", "InkwellTest_A", "node-7", "hello \xFF from as2");
		srv.SendTrace("error", "runtime", "", "", "second");
		srv.SendMenuState("InkwellTest_A", true);
		auto t = ed.WaitType("trace");
		CHECK(t && (*t)["source"] == "as2" && (*t)["node"] == "node-7" && (*t)["menu"] == "InkwellTest_A" && (*t)["level"] == "info");
		t = ed.WaitType("trace");
		CHECK(t && (*t)["text"] == "second" && (*t)["level"] == "error" && !t->contains("node"));
		const auto ms = ed.WaitType("menuState");
		CHECK(ms && (*ms)["open"] == true);

		srv.SendWatch("InkwellTest_A", "g1", "menu", "count", dev::Scalar{ 3.0 });
		const auto w = ed.WaitType("watch");
		CHECK(w && (*w)["name"] == "count" && (*w)["value"] == 3.0 && (*w)["scope"] == "menu");

		for (int i = 0; i < 100; ++i) srv.SendPin("InkwellTest_A", "n1", "out", dev::Scalar{ static_cast<double>(i) });
		int  pins = 0;
		double last = -1;
		for (auto p = ed.WaitType("pin", 1500); p; p = ed.WaitType("pin", 400)) {
			++pins;
			last = (*p)["value"].get<double>();
			if (last == 99) break;
		}
		CHECK(pins >= 1 && pins <= 10 && last == 99);  // coalesced: far fewer than 100, and the latest value arrived

		srv.SendPin("InkwellTest_A", "n2", "txt", dev::Scalar{ std::string(2000, 'q') });
		const auto lp = ed.WaitType("pin");
		CHECK(lp && (*lp)["value"].get<std::string>().size() == 512);

		ed.SendJson({ { "type", "reload" }, { "id", 41 }, { "menu", "InkwellTest_B" } });
		for (int i = 0; i < 50; ++i) {
			{
				std::lock_guard lk(host.m);
				if (!host.reloads.empty()) break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		{
			std::lock_guard lk(host.m);
			CHECK(host.reloads.size() == 1 && host.reloads[0].first == 41 && host.reloads[0].second == "InkwellTest_B");
		}
		srv.SendReloadAck(41, true, { "InkwellTest_B" }, {}, 12, "");
		const auto ack = ed.WaitType("reloadAck");
		CHECK(ack && (*ack)["id"] == 41 && (*ack)["ok"] == true && (*ack)["reloaded"][0] == "InkwellTest_B");
	}
	{  // flood: 5000 traces at once overflow the 1000-message queue; the editor is told, and the server stays responsive
		for (int i = 0; i < 5000; ++i) srv.SendTrace("debug", "runtime", "", "", "flood " + std::to_string(i));
		int  seen = 0;
		bool dropNote = false;
		for (auto t = ed.WaitType("trace", 2000); t; t = ed.WaitType("trace", 700)) {
			++seen;
			dropNote = dropNote || (*t)["text"].get<std::string>().find("dropped") != std::string::npos;
		}
		CHECK(seen < 5000 && seen >= 900 && dropNote);
		ed.SendJson({ { "type", "ping" } });
		CHECK(ed.WaitType("pong"));
	}
	{  // oversize message: close 1009 (the header alone is enough); the slot is free again afterwards
		Client big;
		CHECK(Login(big, port, token) == false);  // busy: the first editor is still connected
		ed.Close();
		for (int i = 0; i < 100 && host.clients != 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
		CHECK(host.clients == 0 && !srv.Connected() && !srv.WantsPins("InkwellTest_A"));  // subscriptions are gone with the client
		Client c;
		CHECK(Login(c, port, token));
		c.SendRaw(Bytes({ 0x81, 0xFF, 0, 0, 0, 0, 0, 0x20, 0, 0, 1, 2, 3, 4 }));  // 2 MiB text frame announced (limit 1 MiB)
		CHECK(c.WaitClose() == 1009);
	}
	{  // protocol violations: unmasked frame (1002), binary (1003), bad UTF-8 (1007)
		Client c;
		CHECK(Login(c, port, token));
		c.SendRaw(Bytes({ 0x81, 0x02, '{', '}' }));
		CHECK(c.WaitClose() == 1002);
		Client d;
		CHECK(Login(d, port, token));
		d.SendFrame(ws::kBinary, "xx");
		CHECK(d.WaitClose() == 1003);
		Client e;
		CHECK(Login(e, port, token));
		e.SendFrame(ws::kText, "\xC0\xAF");
		CHECK(e.WaitClose() == 1007);
		Client f;
		CHECK(Login(f, port, token));
		f.SendRaw(std::string(3000, '\xFF'));  // garbage
		CHECK(f.WaitClose() != -1);
		Client g;
		CHECK(Login(g, port, token));  // still serving
	}
	srv.Stop();
	CHECK(host.clients == 0);
}

static void TestServerTimers()
{
	const std::string token = GenerateToken();
	{  // no hello within the handshake window: 1008
		FakeHost       host;
		auto           cfg = Cfg(token);
		cfg.handshakeMs = 300;
		dev::DevServer srv(host, cfg);
		std::string    err;
		CHECK(srv.Start(err));
		Client c;
		CHECK(c.Connect(srv.Port()) && c.Upgrade(srv.Port(), GoodReq(srv.Port())).find(" 101 ") != std::string::npos);
		CHECK(c.WaitClose(2000) == 1008);
		Client d;  // connect and say nothing at all: the socket is dropped
		CHECK(d.Connect(srv.Port()));
		CHECK(d.WaitClose(2000) == 0);
	}
	{  // ping every 100 ms; a client that never answers is dropped (1001), one that answers stays
		FakeHost       host;
		auto           cfg = Cfg(token);
		cfg.pingEveryMs = 100;
		cfg.pongTimeoutMs = 600;
		dev::DevServer srv(host, cfg);
		std::string    err;
		CHECK(srv.Start(err));
		Client mute;
		CHECK(Login(mute, srv.Port(), token));
		ws::Message m;
		bool        gotPing = false;
		int         code = -1;
		while (mute.Next(m, 2000)) {
			if (m.opcode == ws::kText && json::parse(m.payload)["type"] == "ping") gotPing = true;
			if (m.opcode == ws::kClose) {
				code = (static_cast<std::uint8_t>(m.payload[0]) << 8) | static_cast<std::uint8_t>(m.payload[1]);
				break;
			}
		}
		CHECK(gotPing && code == 1001);
		for (int i = 0; i < 100 && host.clients != 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
		CHECK(host.clients == 0);
		Client good;
		CHECK(Login(good, srv.Port(), token));
		const auto t0 = std::chrono::steady_clock::now();
		int        pings = 0;
		while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1800)) {
			const auto j = good.NextJson(300);
			if (j && j->value("type", "") == "ping") {
				++pings;
				good.SendJson({ { "type", "pong" }, { "id", (*j)["id"] } });
			}
		}
		CHECK(pings >= 5 && srv.Connected());
	}
	{  // five wrong tokens: new connections are refused for a minute
		FakeHost       host;
		dev::DevServer srv(host, Cfg(token));
		std::string    err;
		CHECK(srv.Start(err));
		for (int i = 0; i < 5; ++i) {
			Client c;
			CHECK(c.Connect(srv.Port()) && c.Upgrade(srv.Port(), GoodReq(srv.Port())).find(" 101 ") != std::string::npos);
			c.SendJson(Hello(std::string(64, 'b')));
			CHECK(c.WaitType("helloAck"));
			c.WaitClose();
		}
		Client c;
		c.Connect(srv.Port());
		CHECK(c.Upgrade(srv.Port(), GoodReq(srv.Port())).find(" 101 ") == std::string::npos);
		Client d;
		d.Connect(srv.Port());
		d.SendJson(Hello(token));  // even the right token is refused while locked out
		CHECK(!Login(d, srv.Port(), token));
	}
	{  // the port is exclusive and loopback only
		FakeHost       host;
		dev::DevServer a(host, Cfg(token));
		std::string    err;
		CHECK(a.Start(err));
		auto cfg = Cfg(token);
		cfg.port = a.Port();
		dev::DevServer b(host, cfg);
		CHECK(!b.Start(err) && !err.empty());
	}
}

// ---- --serve: a fake host for scripts/dev-client.mjs ------------------------------------------------------------------------
static int Serve(int port, const std::string& token)
{
	FakeHost host;
	host.verbose = true;
	auto cfg = Cfg(token);
	cfg.port = port;
	dev::DevServer srv(host, cfg);
	std::string    err;
	if (!srv.Start(err)) {
		std::cerr << err << "\n";
		return 1;
	}
	srv.SetMenus({ { "DemoMenu", "Interface/Inkwell/Demo/DemoMenu.swf", false } });
	std::cout << "fake dev server on 127.0.0.1:" << srv.Port() << ", token " << MaskToken(token) << " (masked)" << std::endl;
	std::size_t handledReloads = 0;
	for (int tick = 0;; ++tick) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (tick % 10 == 0) srv.SendTrace("info", "runtime", "", "", "heartbeat " + std::to_string(tick / 10));
		if (srv.WantsPins("DemoMenu")) srv.SendPin("DemoMenu", "node-1", "value", dev::Scalar{ static_cast<double>(tick) });
		if (srv.WantsWatch("DemoMenu") && tick % 10 == 0) srv.SendWatch("DemoMenu", "main", "menu", "counter", dev::Scalar{ static_cast<double>(tick / 10) });
		std::vector<std::pair<double, std::optional<std::string>>> pending;
		{
			std::lock_guard lk(host.m);
			while (handledReloads < host.reloads.size()) pending.push_back(host.reloads[handledReloads++]);
		}
		for (auto& [id, menu] : pending) {
			if (menu && *menu != "DemoMenu") srv.SendError("unknown-menu", "no menu " + *menu, id);
			else srv.SendReloadAck(id, true, { "DemoMenu" }, {}, 5, "");
		}
	}
}

int main(int argc, char** argv)
{
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	if (argc >= 4 && std::string(argv[1]) == "--serve") return Serve(std::atoi(argv[2]), argv[3]);
	const fs::path assets = argc > 1 ? argv[1] : "test-assets";
	TestCrypto();
	TestFrames();
	TestUpgrade();
	TestCodec();
	TestQueueAndLimits();
	TestIniUpsert();
	TestScan(assets);
	TestWatcher();
	TestServer();
	TestServerTimers();
	std::cout << g_checks << " checks, " << g_failed << " failed\n";
	return g_failed == 0 ? 0 : 1;
}
