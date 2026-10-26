#include "DevProtocol.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <utility>

namespace inkwell::dev
{
	using json = nlohmann::json;

	namespace
	{
		std::string Dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }  // invalid UTF-8 from the game never throws

		json Id(double id)
		{
			if (std::floor(id) == id && std::fabs(id) < 9e15) return static_cast<std::int64_t>(id);
			return id;
		}

		json ScalarJson(const Scalar& v)
		{
			if (const auto* b = std::get_if<bool>(&v)) return *b;
			if (const auto* d = std::get_if<double>(&v)) return std::isfinite(*d) ? json(*d) : json(nullptr);
			if (const auto* s = std::get_if<std::string>(&v)) return *s;
			return nullptr;
		}

		bool IsInt(const json& j) { return j.is_number_integer() || (j.is_number_float() && std::floor(j.get<double>()) == j.get<double>()); }
	}

	ParseOutcome ParseEditorMessage(std::string_view text)
	{
		ParseOutcome out;
		const json   j = json::parse(text.begin(), text.end(), nullptr, false);
		if (j.is_discarded() || !j.is_object()) {
			out.error = "not a JSON object";
			return out;
		}
		const auto t = j.find("type");
		if (t == j.end() || !t->is_string()) {
			out.error = "missing string field 'type'";
			return out;
		}
		const std::string type = t->get<std::string>();
		const auto        str = [&](const char* k, std::string& dst) {
			const auto it = j.find(k);
			if (it == j.end() || !it->is_string()) return false;
			dst = it->get<std::string>();
			return true;
		};
		const auto optBool = [&](const char* k, bool& dst) {
			const auto it = j.find(k);
			if (it == j.end()) return true;
			if (!it->is_boolean()) return false;
			dst = it->get<bool>();
			return true;
		};
		const auto optId = [&](std::optional<double>& dst) {
			const auto it = j.find("id");
			if (it == j.end()) return true;
			if (!it->is_number()) return false;
			dst = it->get<double>();
			return true;
		};

		if (type == "hello") {
			Hello h;
			const auto pv = j.find("protocolVersion"), sv = j.find("specVersion");
			if (!str("token", h.token) || !str("editorVersion", h.editorVersion) || pv == j.end() || sv == j.end() || !IsInt(*pv) || !IsInt(*sv)) {
				out.error = "hello needs string token, string editorVersion, integer protocolVersion and specVersion";
				return out;
			}
			h.protocolVersion = static_cast<int>(pv->get<double>());
			h.specVersion = static_cast<int>(sv->get<double>());
			out.msg = std::move(h);
		} else if (type == "subscribe" || type == "unsubscribe") {
			Subscribe s;
			s.on = type == "subscribe";
			if (!str("menu", s.menu) || !optBool("watch", s.watch) || !optBool("pins", s.pins)) {
				out.error = type + " needs a string menu and boolean watch/pins";
				return out;
			}
			out.msg = std::move(s);
		} else if (type == "reload") {
			Reload r;
			const auto id = j.find("id");
			if (id == j.end() || !id->is_number()) {
				out.error = "reload needs a numeric id";
				return out;
			}
			r.id = id->get<double>();
			if (const auto m = j.find("menu"); m != j.end()) {
				if (!m->is_string()) {
					out.error = "reload menu must be a string";
					return out;
				}
				r.menu = m->get<std::string>();
			}
			out.msg = std::move(r);
		} else if (type == "ping" || type == "pong") {
			std::optional<double> id;
			if (!optId(id)) {
				out.error = type + " id must be a number";
				return out;
			}
			if (type == "ping") out.msg = Ping{ id };
			else out.msg = Pong{ id };
		} else {
			out.msg = Unknown{ type };
		}
		return out;
	}

	std::string MakeHelloAck(const HelloAckInfo& i)
	{
		json m = json::array();
		for (const auto& e : i.menus) m.push_back({ { "name", e.name }, { "swf", e.swf }, { "open", e.open } });
		json j = { { "type", "helloAck" }, { "ok", i.ok }, { "runtimeVersion", i.runtimeVersion }, { "runtimeVersionString", i.runtimeVersionString },
			{ "protocolVersion", i.protocolVersion }, { "specVersion", i.specVersion }, { "gameVersion", i.gameVersion }, { "menus", m } };
		if (!i.error.empty()) j["error"] = i.error;
		return Dump(j);
	}

	std::string MakeMenuState(std::string_view menu, bool open) { return Dump({ { "type", "menuState" }, { "menu", menu }, { "open", open } }); }

	std::string MakeTrace(std::int64_t ts, std::string_view level, std::string_view source, std::string_view menu, std::string_view node, std::string_view graph, std::string_view text)
	{
		json j = { { "type", "trace" }, { "ts", ts }, { "level", level }, { "source", source }, { "text", text } };
		if (!menu.empty()) j["menu"] = menu;
		if (!node.empty()) j["node"] = node;
		if (!graph.empty()) j["graph"] = graph;
		return Dump(j);
	}

	std::string MakeWatch(std::string_view menu, std::string_view graph, std::string_view scope, std::string_view name, const Scalar& value)
	{
		return Dump({ { "type", "watch" }, { "menu", menu }, { "graph", graph }, { "scope", scope }, { "name", name }, { "value", ScalarJson(value) } });
	}

	std::string MakePin(std::string_view menu, std::string_view node, std::string_view pin, const Scalar& value)
	{
		return Dump({ { "type", "pin" }, { "menu", menu }, { "node", node }, { "pin", pin }, { "value", ScalarJson(value) } });
	}

	std::string MakeReloadAck(double id, bool ok, const std::vector<std::string>& reloaded, const std::vector<SkippedMenu>& skipped, std::int64_t ms, std::string_view error)
	{
		json s = json::array();
		for (const auto& e : skipped) s.push_back({ { "name", e.name }, { "reason", e.reason } });
		json j = { { "type", "reloadAck" }, { "id", Id(id) }, { "ok", ok }, { "reloaded", reloaded }, { "skipped", s }, { "ms", ms } };
		if (!error.empty()) j["error"] = error;
		return Dump(j);
	}

	std::string MakeError(std::string_view code, std::string_view message, std::optional<double> id)
	{
		json j = { { "type", "error" }, { "code", code }, { "message", message } };
		if (id) j["id"] = Id(*id);
		return Dump(j);
	}

	std::string MakePing(std::optional<double> id)
	{
		json j = { { "type", "ping" } };
		if (id) j["id"] = Id(*id);
		return Dump(j);
	}

	std::string MakePong(std::optional<double> id)
	{
		json j = { { "type", "pong" } };
		if (id) j["id"] = Id(*id);
		return Dump(j);
	}

	std::string CutPinText(std::string s)
	{
		// characters, not bytes: never cut inside a UTF-8 sequence
		std::size_t chars = 0, i = 0;
		while (i < s.size()) {
			if (chars == static_cast<std::size_t>(kPinValueChars)) {
				s.resize(i);
				break;
			}
			++i;
			while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
			++chars;
		}
		return s;
	}

	// ---- OutQueue ---------------------------------------------------------------------------------------------------
	void OutQueue::Push(Kind k, std::string json)
	{
		std::lock_guard lk(_m);
		if (_q.size() >= _cap) {
			// drop the oldest message of the lowest kind present; if the new one is lower than everything queued, it is the one dropped
			auto victim = _q.end();
			for (auto it = _q.begin(); it != _q.end(); ++it) {
				if (victim == _q.end() || it->first < victim->first) victim = it;
			}
			if (victim == _q.end() || k < victim->first) {
				++_dropped;
				return;
			}
			_q.erase(victim);
			++_dropped;
		}
		_q.emplace_back(k, std::move(json));
	}

	bool OutQueue::Pop(std::string& out)
	{
		std::lock_guard lk(_m);
		if (_q.empty()) return false;
		out = std::move(_q.front().second);
		_q.pop_front();
		return true;
	}

	std::uint64_t OutQueue::TakeDropped()
	{
		std::lock_guard lk(_m);
		return std::exchange(_dropped, 0);
	}

	std::size_t OutQueue::Size() const
	{
		std::lock_guard lk(_m);
		return _q.size();
	}

	void OutQueue::Clear()
	{
		std::lock_guard lk(_m);
		_q.clear();
		_dropped = 0;
	}

	// ---- StreamLimiter ----------------------------------------------------------------------------------------------
	bool StreamLimiter::Submit(const std::string& key, std::string json)
	{
		std::lock_guard lk(_m);
		auto            it = _slots.find(key);
		if (it == _slots.end()) {
			if (_slots.size() >= _maxKeys) return false;
			it = _slots.emplace(key, Slot{}).first;
		}
		it->second.pending = std::move(json);
		it->second.has = true;
		return true;
	}

	std::vector<std::pair<std::string, std::string>> StreamLimiter::Flush(std::int64_t now)
	{
		std::lock_guard          lk(_m);
		std::vector<std::pair<std::string, std::string>> out;
		if (now - _windowStart >= 1000) {
			_windowStart = now;
			_inWindow = 0;
		}
		const std::int64_t gap = 1000 / (_perKey > 0 ? _perKey : 1);
		for (auto& [k, s] : _slots) {
			if (!s.has || now - s.lastSent < gap) continue;
			if (_inWindow >= _total) break;
			out.emplace_back(k, std::move(s.pending));
			s.pending.clear();
			s.has = false;
			s.lastSent = now;
			++_inWindow;
		}
		return out;
	}

	void StreamLimiter::Clear()
	{
		std::lock_guard lk(_m);
		_slots.clear();
		_inWindow = 0;
	}

	// ---- Lockout ----------------------------------------------------------------------------------------------------
	void Lockout::Fail(std::int64_t now)
	{
		_fails.push_back(now);
		while (!_fails.empty() && now - _fails.front() > 60'000) _fails.pop_front();
		if (_fails.size() >= static_cast<std::size_t>(kFailedTokens)) {
			_until = now + kLockoutMs;
			_fails.clear();
		}
	}
}
