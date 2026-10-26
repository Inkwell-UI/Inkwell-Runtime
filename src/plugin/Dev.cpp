#include "Dev.h"

#include "Menu.h"

#include "core/Crypto.h"
#include "core/DevServer.h"
#include "core/DirWatcher.h"

#include <spdlog/sinks/base_sink.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <fstream>
#include <sstream>
#include <thread>

namespace inkwell::devmode
{
	namespace fs = std::filesystem;
	using Clock = std::chrono::steady_clock;

	namespace
	{
		const fs::path kIni = fs::path("Data") / "SKSE" / "Plugins" / INKWELL_PRODUCT_NAME / "Inkwell.ini";

		std::atomic<dev::DevServer*> g_server{ nullptr };  // set once, never freed (the DLL is never unloaded; joining threads in static destructors can hang)
		bool                         g_sinkInstalled{ false };

		const char* LevelName(spdlog::level::level_enum l)
		{
			switch (l) {
			case spdlog::level::trace: return "trace";
			case spdlog::level::debug: return "debug";
			case spdlog::level::warn: return "warn";
			case spdlog::level::err:
			case spdlog::level::critical: return "error";
			default: return "info";
			}
		}

		/// Every spdlog line becomes a `trace` message (source runtime). Lines that the AS2 bridge forwards itself (with menu and node) are skipped.
		class ForwardSink : public spdlog::sinks::base_sink<std::mutex>
		{
		protected:
			void sink_it_(const spdlog::details::log_msg& m) override
			{
				thread_local bool inSink = false;
				if (inSink) return;
				inSink = true;
				if (auto* s = g_server.load(); s && s->Connected()) {
					const std::string_view p(m.payload.data(), m.payload.size());
					if (p.substr(0, 7) != "[AS2] [") s->SendTrace(LevelName(m.level), "runtime", "", "", p);
				}
				inSink = false;
			}
			void flush_() override {}
		};

		// ---- host: callbacks arrive on the server thread; they only queue work to the game thread ----
		class Host : public dev::DevHost
		{
		public:
			void Log(int level, const std::string& text) override
			{
				static constexpr spdlog::level::level_enum map[] = { spdlog::level::trace, spdlog::level::debug, spdlog::level::info, spdlog::level::warn, spdlog::level::err };
				spdlog::default_logger()->log(map[std::clamp(level, 0, 4)], "[dev] {}", text);
			}
			void OnClient(bool) override;
			void OnSubscribe(const std::string& menu, bool watch, bool pins, bool watchWasOff) override;
			void OnReload(double id, const std::optional<std::string>& menu) override;
		};
		Host g_host;

		void RefreshMenus(const MenuEntry* overrideEntry = nullptr, bool overrideOpen = false)
		{
			auto* s = g_server.load();
			if (!s) return;
			auto&                       rt = Runtime::Get();
			const auto                  open = rt.OpenMenus();
			std::vector<dev::MenuInfo>  list;
			for (auto* e : rt.AllEntries()) {
				if (!e->Active()) continue;
				bool o = std::find(open.begin(), open.end(), e) != open.end();
				if (e == overrideEntry) o = overrideOpen;
				list.push_back({ e->def.name, e->def.swf, o });
			}
			s->SetMenus(std::move(list));
		}

		// ---- hot reload jobs (game thread only) ----
		struct Job
		{
			std::optional<double> id;  // editor request; none = file watcher
			bool                  force{ false };
			std::string           only;
			Clock::time_point     start{ Clock::now() };

			bool                 planned{ false };
			Runtime::ReloadPlan  plan;
			std::size_t          idx{ 0 };
			bool                 waiting{ false }, reopen{ false };
			Clock::time_point    waitSince;
			std::string          reopenName;
			std::vector<std::string>        reloaded;
			std::vector<dev::SkippedMenu>   skipped;
			std::string                     error;
		};
		std::deque<std::shared_ptr<Job>> g_jobs;
		bool                             g_running = false;
		std::atomic<bool>                g_watchQueued{ false };

		constexpr auto kCloseTimeout = std::chrono::milliseconds(3000);

		void Step();

		// A task that re-adds itself runs again inside the same frame, so the engine never gets to process the queued kHide (seen in game: the close
		// came 3 s late, right after the timeout). Wait a few frames on a helper thread, then hop back onto the game thread.
		void Requeue()
		{
			std::thread([] {
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				SKSE::GetTaskInterface()->AddTask([] { Step(); });
			}).detach();
		}

		void Finish(Job& j)
		{
			RefreshMenus();
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - j.start).count();
			bool       failed = !j.error.empty();
			for (const auto& s : j.skipped) failed = failed || s.reason != "unchanged";
			std::string summary = "hot reload " + std::string(j.id ? "(editor request)" : "(file watcher)") + ": reloaded " + std::to_string(j.reloaded.size()) + ", skipped " + std::to_string(j.skipped.size()) + ", " + std::to_string(ms) + " ms";
			for (const auto& n : j.reloaded) summary += "; reloaded " + n;
			for (const auto& s : j.skipped) summary += "; skipped " + s.name + " (" + s.reason + ")";
			if (!j.error.empty()) summary += "; " + j.error;
			if (!j.id && j.reloaded.empty() && !failed) {
				log::debug("{}", summary);  // the watcher saw nothing that changes a menu
			} else {
				log::info("{}", summary);
			}
			if (auto* s = g_server.load()) {
				if (j.id) {
					s->SendReloadAck(*j.id, !failed, j.reloaded, j.skipped, ms, j.error);
					if (j.plan.unknownMenu) s->SendError("unknown-menu", j.error, j.id);
					else if (failed && j.reloaded.empty()) s->SendError("reload-failed", summary, j.id);
				}
			}
			g_jobs.pop_front();
			g_running = false;
		}

		void Pump()
		{
			if (g_running || g_jobs.empty()) return;
			g_running = true;
			Step();
		}

		void Step()
		{
			if (g_jobs.empty()) {
				g_running = false;
				return;
			}
			auto  jp = g_jobs.front();
			Job&  j = *jp;
			auto& rt = Runtime::Get();
			try {
				if (!j.reopenName.empty()) {  // one tick after the engine destroyed the old instance
					rt.OpenMenu(j.reopenName, "hot reload", false);
					j.reopenName.clear();
				}
				if (!j.planned) {
					j.planned = true;
					j.plan = rt.PlanReload(j.only, j.force);
					for (auto& [n, why] : j.plan.skipped) j.skipped.push_back({ n, why });
					if (j.plan.unknownMenu) j.error = "no menu '" + j.only + "' is registered or has a menu file";
				}
				while (j.idx < j.plan.actions.size()) {
					auto&      a = j.plan.actions[j.idx];
					MenuEntry* e = rt.FindAny(a.name);
					if (!j.waiting && e) {
						const auto open = rt.OpenMenus();
						if (std::find(open.begin(), open.end(), e) != open.end()) {
							// S6: close it (kHide, so On Close runs), wait until the engine has destroyed it
							log::info("hot reload: closing '{}' to apply the new files", a.name);
							j.reopen = a.kind != Runtime::ReloadAction::Kind::kRemove;
							rt.CloseMenu(a.name, "hot reload");
							j.waiting = true;
							j.waitSince = Clock::now();
						}
					}
					if (j.waiting) {
						const auto open = rt.OpenMenus();
						if (e && std::find(open.begin(), open.end(), e) != open.end()) {
							if (Clock::now() - j.waitSince > kCloseTimeout) {
								j.skipped.push_back({ a.name, "the engine did not destroy the open menu within 3 s; not reloaded" });
								j.waiting = false;
								j.reopen = false;
								++j.idx;
								continue;
							}
							Requeue();
							return;
						}
						j.waiting = false;
					}
					switch (a.kind) {
					case Runtime::ReloadAction::Kind::kUpdate:
						if (e) {
							rt.ApplyUpdate(*e, std::move(*a.candidate));
							j.reloaded.push_back(a.name);
							if (j.reopen) j.reopenName = a.name;
						}
						break;
					case Runtime::ReloadAction::Kind::kAdd: {
						std::string why;
						if (rt.AddMenu(std::move(*a.candidate), why)) j.reloaded.push_back(a.name);
						else j.skipped.push_back({ a.name, why });
						break;
					}
					case Runtime::ReloadAction::Kind::kRemove:
						if (e) {
							rt.RemoveMenu(*e);
							j.reloaded.push_back(a.name + " (removed)");
						}
						break;
					}
					j.reopen = false;
					++j.idx;
					if (!j.reopenName.empty()) {
						Requeue();
						return;
					}
				}
				Finish(j);
			} catch (const std::exception& ex) {
				j.error = std::string("exception: ") + ex.what();
				log::error("hot reload failed: {}", j.error);
				Finish(j);
			} catch (...) {
				j.error = "unknown exception";
				log::error("hot reload failed: unknown exception");
				Finish(j);
			}
			Pump();
		}

		void Enqueue(std::shared_ptr<Job> j)
		{
			SKSE::GetTaskInterface()->AddTask([j] {
				g_jobs.push_back(j);
				Pump();
			});
		}

		void Host::OnClient(bool connected)
		{
			log::info("dev: editor {}", connected ? "connected" : "disconnected");
			if (connected) SKSE::GetTaskInterface()->AddTask([] { RefreshMenus(); });
		}

		void Host::OnSubscribe(const std::string& menu, bool watch, bool, bool watchWasOff)
		{
			if (!watch || !watchWasOff) return;
			SKSE::GetTaskInterface()->AddUITask([menu] {
				if (const auto m = FindOpenMenu(menu); m && m->IsOpenPhase()) m->DumpVariables();
			});
		}

		void Host::OnReload(double id, const std::optional<std::string>& menu)
		{
			auto j = std::make_shared<Job>();
			j->id = id;
			j->force = true;  // the editor just deployed: reload even if the file stamps look unchanged
			j->only = menu.value_or("");
			Enqueue(std::move(j));
		}

		void OnFilesChanged(const std::set<std::string>& paths)
		{
			bool relevant = false;
			for (const auto& p : paths) {
				const std::string l = LowerCopy(p);
				const auto        ext = fs::path(l).extension().string();
				if (l.find("inkwell.ini") != std::string::npos && fs::path(l).filename() == "inkwell.ini") continue;
				if (ext == ".json" || ext == ".ini" || ext == ".swf") relevant = true;
			}
			if (!relevant || g_watchQueued.exchange(true)) return;
			log::debug("file watcher: {} changed path(s); checking the menus", paths.size());
			auto j = std::make_shared<Job>();
			j->force = false;
			SKSE::GetTaskInterface()->AddTask([j] {
				g_watchQueued = false;
				g_jobs.push_back(j);
				Pump();
			});
		}

		std::string ReadText(const fs::path& p)
		{
			std::ifstream     in(p, std::ios::binary);
			std::stringstream ss;
			ss << in.rdbuf();
			return ss.str();
		}
	}

	bool Active() { return Runtime::Get().config.devMode; }

	void InstallLogForwarder()
	{
		if (!Active() || g_sinkInstalled) return;
		spdlog::default_logger()->sinks().push_back(std::make_shared<ForwardSink>());
		g_sinkInstalled = true;
	}

	void Start()
	{
		auto& rt = Runtime::Get();
		if (!rt.config.devMode) {
			log::info("dev mode is off ([Dev] DevMode is not 1 in Inkwell.ini): no socket, no file watcher, watch/pin are no-ops");
			return;
		}
		try {
			const std::string bar(78, '=');
			log::warn("{}", bar);
			log::warn("INKWELL DEV MODE IS ON ([Dev] DevMode=1 in {}).", kIni.string());
			log::warn("A debug server accepts an editor on 127.0.0.1:{} with a token, and menu files are hot reloaded.", rt.config.port);
			log::warn("Players should not run this: remove DevMode=1 (or the file) for a release setup.");
			log::warn("{}", bar);

			std::string token = rt.config.token;
			if (!IsValidToken(token)) {
				token = GenerateToken();
				if (token.empty()) {
					log::error("dev mode: the OS random generator failed; the dev server is not started");
					return;
				}
				std::error_code ec;
				fs::create_directories(kIni.parent_path(), ec);
				const std::string text = UpsertIniValue(fs::exists(kIni, ec) ? ReadText(kIni) : std::string(), "Dev", "Token", token);
				std::ofstream     out(kIni, std::ios::binary | std::ios::trunc);
				out << text;
				out.flush();
				if (out) log::info("dev mode: generated a new token and wrote it to {} ([Dev] Token); the editor reads it from there", kIni.string());
				else log::error("dev mode: could not write the token to {}; the editor cannot read it (put a 64 hex digit [Dev] Token there yourself)", kIni.string());
			}

			dev::ServerConfig cfg;
			cfg.port = rt.config.port;
			cfg.token = token;
			cfg.runtimeVersion = Pack(kRuntimeVersion);
			cfg.runtimeVersionString = ToString(kRuntimeVersion);
			cfg.gameVersion = REL::Module::get().version().string();
			cfg.protocolVersion = kRuntimeProtocol;
			cfg.specVersion = kSpecVersion;
			auto*       server = new dev::DevServer(g_host, cfg);  // leaked on purpose
			std::string err;
			if (server->Start(err)) {
				g_server = server;
				RefreshMenus();
				log::warn("dev server on 127.0.0.1:{}, token {}", server->Port(), MaskToken(token));
			} else {
				log::error("dev server not started: {}", err);
				delete server;
			}

			if (rt.config.watch) {
				static DirWatcher watcher;  // leaked at exit like the server
				std::vector<std::string> skipped;
				const bool ok = watcher.Start({ { fs::path("Data") / "SKSE" / "Plugins" / INKWELL_PRODUCT_NAME, true }, { fs::path("Data") / "Interface", true } }, 300, OnFilesChanged, skipped);
				for (const auto& s : skipped) log::warn("file watcher: cannot watch {}", s);
				log::info("file watcher on the menu JSON, INIs and SWFs: {}", ok ? "running (300 ms debounce)" : "not running");
			} else {
				log::info("file watcher is off ([Dev] Watch=0); the editor's reload request still works");
			}
		} catch (const std::exception& e) {
			log::error("dev mode start failed: {}", e.what());
		} catch (...) {
			log::error("dev mode start failed: unknown exception");
		}
	}

	void OnMenuState(const MenuEntry& e, bool open)
	{
		if (auto* s = g_server.load()) {
			s->SendMenuState(e.def.name, open);
			RefreshMenus(&e, open);
		}
	}

	void OnMenuOpened(MenuEntry& e)
	{
		auto* s = g_server.load();
		if (s && s->WantsWatch(e.def.name)) {
			if (const auto m = FindOpenMenu(e.def.name)) m->DumpVariables();
		}
	}

	void Trace(std::string_view level, std::string_view menu, std::string_view node, std::string_view text)
	{
		if (auto* s = g_server.load()) {
			const std::string l(level);
			s->SendTrace(l == "trace" || l == "debug" || l == "warn" || l == "error" ? l : "info", "as2", menu, node, text);
		}
	}

	bool WantsWatch(std::string_view menu)
	{
		auto* s = g_server.load();
		return s && s->WantsWatch(menu);
	}

	bool WantsPins(std::string_view menu)
	{
		auto* s = g_server.load();
		return s && s->WantsPins(menu);
	}

	void Watch(std::string_view menu, std::string_view graph, std::string_view scope, std::string_view name, const dev::Scalar& v)
	{
		if (auto* s = g_server.load()) {
			if (scope != "component" && scope != "menu" && scope != "saved") return;
			s->SendWatch(menu, graph, scope, name, v);
		}
	}

	void Pin(std::string_view menu, std::string_view node, std::string_view pin, const dev::Scalar& v)
	{
		if (auto* s = g_server.load()) s->SendPin(menu, node, pin, v);
	}
}
