#include "Runtime.h"

#include "Menu.h"
#include "Messaging.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace inkwell
{
	namespace fs = std::filesystem;

	namespace
	{
		const fs::path kBaseDir = fs::path("Data") / "SKSE" / "Plugins" / INKWELL_PRODUCT_NAME;

		std::string ReadAll(const fs::path& p, bool& ok)
		{
			std::ifstream in(p, std::ios::binary);
			ok = static_cast<bool>(in);
			std::stringstream ss;
			ss << in.rdbuf();
			return ss.str();
		}

		std::vector<fs::path> ListFiles(const fs::path& dir, std::string_view ext)
		{
			std::vector<fs::path> out;
			std::error_code       ec;
			for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
				std::error_code ec2;
				if (!it->is_regular_file(ec2)) continue;
				if (LowerCopy(it->path().extension().string()) != ext) continue;
				out.push_back(it->path());
			}
			// case-insensitive file-name order (S1 rule 2)
			std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) { return LowerCopy(a.filename().string()) < LowerCopy(b.filename().string()); });
			return out;
		}

		void LogProblem(const std::string& where, const Problem& p)
		{
			switch (p.level) {
			case Problem::Level::kDebug: log::debug("{}: {}", where, p.message); break;
			case Problem::Level::kWarn: log::warn("{}: {}", where, p.message); break;
			case Problem::Level::kError: log::error("{}: {}", where, p.message); break;
			}
		}
	}

	Runtime& Runtime::Get()
	{
		static Runtime r;
		return r;
	}

	// ---------------------------------------------------------------------------------------------------------------
	void Runtime::LoadRuntimeConfig()
	{
		const fs::path path = kBaseDir / "Inkwell.ini";
		ParsedIni      ini;
		bool           ok = false;
		const auto     text = ReadAll(path, ok);
		if (ok) {
			ini = ParseIni(text, IniKind::kRuntime);
			log::info("read {}", path.string());
		} else {
			log::info("no {} (that is fine): built-in defaults", path.string());
		}

		std::string level = ini.logLevel.value_or(INKWELL_DEFAULT_LOG_LEVEL);
		const auto  lvl = spdlog::level::from_str(level);
		spdlog::default_logger()->set_level(lvl);
		spdlog::default_logger()->flush_on(lvl <= spdlog::level::debug ? spdlog::level::trace : spdlog::level::warn);
		config.logLevel = level;
		config.devMode = ini.devMode.value_or(false);
		config.port = ini.port.value_or(8788);
		config.watch = ini.watch.value_or(true);
		config.hasToken = ini.token.has_value();
		log::info("log level '{}' ({}); Inkwell.ini [Dev] DevMode={} Port={} Watch={}", level, ini.logLevel ? "from Inkwell.ini" : "built-in default", config.devMode, config.port, config.watch);
		for (const auto& p : ini.problems) log::warn("{}:{} [{}] {}: {}", path.string(), p.line, p.section, p.key, p.message);
		if (config.devMode) log::warn("[Dev] DevMode=1 is set, but the dev server (hot reload, WebSocket) is not part of this build; it arrives in a later phase. Dev mode stays off.");
	}

	// ---------------------------------------------------------------------------------------------------------------
	void Runtime::LoadMenus()
	{
		std::error_code ec;
		log::info("scanning {} (cwd {})", kBaseDir.string(), fs::current_path(ec).string());

		// 1. per-mod INIs: Data/SKSE/Plugins/Inkwell/<modId>.ini (Inkwell.ini is the runtime-wide file, read at plugin load)
		std::vector<std::pair<fs::path, std::shared_ptr<const ParsedIni>>> inis;
		for (const auto& p : ListFiles(kBaseDir, ".ini")) {
			if (LowerCopy(p.filename().string()) == "inkwell.ini") continue;
			bool        ok = false;
			const auto  text = ReadAll(p, ok);
			if (!ok) {
				log::warn("{}: cannot be read; ignored", p.string());
				continue;
			}
			auto parsed = std::make_shared<ParsedIni>(ParseIni(text, IniKind::kMod));
			for (const auto& pr : parsed->problems) log::warn("{}:{} [{}] {}: {}", p.string(), pr.line, pr.section, pr.key, pr.message);
			log::info("{}: {} menu section(s)", p.string(), parsed->menus.size());
			inis.emplace_back(p, std::move(parsed));
		}

		// 2. menu JSON
		const fs::path dir = kBaseDir / "menus";
		if (!fs::is_directory(dir, ec)) {
			log::warn("menus directory not found: {} (no menus to register)", dir.string());
			return;
		}
		std::map<std::string, std::string> byName;  // lower-case name -> file
		int                                 seen = 0, refused = 0;
		for (const auto& p : ListFiles(dir, ".json")) {
			++seen;
			const std::string file = p.string();
			bool              ok = false;
			const auto        text = ReadAll(p, ok);
			if (!ok) {
				log::error("{}: skipped, cannot be read", file);
				continue;
			}
			auto r = ParseMenuDef(text);
			for (const auto& pr : r.problems) {
				if (pr.level == Problem::Level::kError) log::error("{}: skipped, {}", file, pr.message);
				else LogProblem(file, pr);
			}
			if (!r.def) continue;
			MenuDef& d = *r.def;

			const std::string key = MenuNameKey(d.name);
			if (const auto it = byName.find(key); it != byName.end()) {
				log::error("{}: skipped, menu name '{}' is already used by {}", file, d.name, it->second);
				continue;
			}
			if (_menus.size() >= MENU_SLOTS) {
				++refused;
				log::error("slot exhaustion: {} not registered ({}: all {} template slots are in use)", d.name, file, MENU_SLOTS);
				continue;
			}
			byName.emplace(key, file);

			auto e = std::make_unique<MenuEntry>();
			e->def = std::move(d);
			e->file = file;
			e->slot = _menus.size();

			// the INI with this menu's section; otherwise <modId>.ini from the swf path Interface/Inkwell/<modId>/...
			for (const auto& [ip, parsed] : inis) {
				if (parsed->FindMenu(e->def.name)) {
					e->modIni = parsed;
					e->iniFile = ip.string();
					break;
				}
			}
			if (!e->modIni) {
				const std::string& bare = e->def.bareSwf;
				if (bare.size() > 8 && LowerCopy(bare.substr(0, 8)) == "inkwell/") {
					const auto slash = bare.find('/', 8);
					if (slash != std::string::npos) {
						const std::string want = LowerCopy(bare.substr(8, slash - 8)) + ".ini";
						for (const auto& [ip, parsed] : inis) {
							if (LowerCopy(ip.filename().string()) == want) {
								e->modIni = parsed;
								e->iniFile = ip.string();
								break;
							}
						}
					}
				}
			}
			e->eff = ResolveMenu(e->def.hotkey, e->modIni ? e->modIni->FindMenu(e->def.name) : nullptr);

			const auto& d2 = e->def;
			log::info("{}: loaded menu '{}' (slot {}) swf='{}' -> LoadMovieEx '{}'", file, d2.name, e->slot, d2.swf, d2.bareSwf);
			log::info("  flags: pausesGame={} cursor={} modal={} closeOnEscape={} inputContext={}{} depth={}", d2.pausesGame, d2.cursor, d2.modal, d2.closeOnEscape,
				d2.inputContext == InputContext::kMenuMode ? "menuMode" : "none", d2.inputContextExplicit ? "" : " (derived from modal)", d2.depth);
			log::info("  triggers: json hotkey={} papyrus={} modEvent='{}'; minRuntime {}", d2.hotkey ? std::to_string(*d2.hotkey) : "none", d2.papyrus, d2.modEvent, d2.minRuntime);
			log::info("  ini: {}; effective hotkey={} uiScale={} offset=({}, {}) theme='{}'", e->iniFile.empty() ? "none" : e->iniFile,
				e->eff.openKey ? std::to_string(*e->eff.openKey) : "none", e->eff.uiScale, e->eff.offsetX, e->eff.offsetY, e->eff.theme);
			_menus.push_back(std::move(e));
		}

		// two menus on one key: both react, which is rarely wanted
		for (std::size_t i = 0; i < _menus.size(); ++i) {
			for (std::size_t j = i + 1; j < _menus.size(); ++j) {
				if (_menus[i]->eff.openKey && _menus[i]->eff.openKey == _menus[j]->eff.openKey) {
					log::warn("hotkey {} opens both '{}' and '{}'", *_menus[i]->eff.openKey, _menus[i]->def.name, _menus[j]->def.name);
				}
			}
		}

		log::info("{} menu file(s) seen, {} loaded", seen, _menus.size());
		if (refused > 0) {
			_refusedSlots = refused;
			std::lock_guard lk(_noteMutex);
			_pendingNotes.push_back("Inkwell: " + std::to_string(refused) + (refused == 1 ? " menu was" : " menus were") + " not registered (limit " + std::to_string(MENU_SLOTS) + " menus). See Inkwell.log.");
		}
	}

	void Runtime::RegisterMenus()
	{
		const auto ui = RE::UI::GetSingleton();
		if (!ui) {
			log::error("UI singleton is null; nothing registered");
			return;
		}
		for (auto& e : _menus) {
			const RE::BSFixedString key(e->def.name.c_str());
			if (ui->menuMap.find(key) != ui->menuMap.end()) {
				log::error("menu '{}' ({}): skipped, the engine already has a menu with this name (vanilla or another plugin)", e->def.name, e->file);
				continue;
			}
			if (!RegisterSlot(e->slot, e->def.name)) {
				log::error("menu '{}': slot {} is out of range", e->def.name, e->slot);
				continue;
			}
			e->registered = true;
			log::info("UI::Register('{}') done (slot {})", e->def.name, e->slot);
		}
	}

	MenuEntry* Runtime::Find(std::string_view name) const
	{
		const std::string key = MenuNameKey(name);
		for (const auto& e : _menus) {
			if (e->registered && MenuNameKey(e->def.name) == key) return e.get();
		}
		return nullptr;
	}

	// ---------------------------------------------------------------------------------------------------------------
	bool Runtime::OpenMenu(std::string_view name, std::string_view source, bool viaPapyrus)
	{
		const auto e = Find(name);
		if (!e) {
			log::warn("open '{}' ({}): unknown menu", name, source);
			return false;
		}
		if (viaPapyrus && !e->def.papyrus) {
			log::warn("open '{}' ({}): refused, the menu JSON has triggers.papyrus=false", e->def.name, source);
			return false;
		}
		const auto ui = RE::UI::GetSingleton();
		const auto q = RE::UIMessageQueue::GetSingleton();
		if (!ui || !q) {
			log::error("open '{}' ({}): UI is not available", e->def.name, source);
			return false;
		}
		if (ui->IsMenuOpen(e->def.name)) {
			log::debug("open '{}' ({}): already open", e->def.name, source);
			return true;
		}
		log::info("open '{}' ({}): queue kShow", e->def.name, source);
		q->AddMessage(e->def.name, RE::UI_MESSAGE_TYPE::kShow, nullptr);
		return true;
	}

	bool Runtime::CloseMenu(std::string_view name, std::string_view source)
	{
		const auto e = Find(name);
		if (!e) {
			log::warn("close '{}' ({}): unknown menu", name, source);
			return false;
		}
		const auto ui = RE::UI::GetSingleton();
		const auto q = RE::UIMessageQueue::GetSingleton();
		if (!ui || !q) return false;
		if (!ui->IsMenuOpen(e->def.name)) {
			log::debug("close '{}' ({}): not open", e->def.name, source);
			return true;
		}
		log::info("close '{}' ({}): queue kHide", e->def.name, source);
		q->AddMessage(e->def.name, RE::UI_MESSAGE_TYPE::kHide, nullptr);
		return true;
	}

	bool Runtime::IsMenuOpen(std::string_view name) const
	{
		const auto e = Find(name);
		const auto ui = RE::UI::GetSingleton();
		return e && ui && ui->IsMenuOpen(e->def.name);
	}

	std::string Runtime::HotkeyBlockedReason(const MenuEntry& e) const
	{
		const auto ui = RE::UI::GetSingleton();
		if (!ui) return "UI singleton missing";
		if (ui->IsMenuOpen(RE::Console::MENU_NAME)) return "the console is open";
		if (ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) return "the main menu is open";
		if (ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) return "a loading screen is shown";
		{
			std::lock_guard lk(_openMutex);
			for (const auto* o : _open) {
				if (o != &e) return "another Inkwell menu is open ('" + o->def.name + "')";
			}
		}
		if (ui->IsApplicationMenuOpen()) return "an application menu (pause, journal, ...) is open";
		if (ui->IsModalMenuOpen()) return "a modal menu is open";
		if (ui->GameIsPaused()) return "the game is paused by a menu";
		return {};
	}

	// ---------------------------------------------------------------------------------------------------------------
	void Runtime::OnMenuCreated(MenuEntry& e)
	{
		std::lock_guard lk(_openMutex);
		_open.push_back(&e);
	}

	void Runtime::OnMenuDestroyed(MenuEntry& e)
	{
		std::lock_guard lk(_openMutex);
		std::erase(_open, &e);
	}

	MenuEntry* Runtime::TopmostOpen() const
	{
		std::lock_guard lk(_openMutex);
		return _open.empty() ? nullptr : _open.back();
	}

	std::vector<MenuEntry*> Runtime::OpenMenus() const
	{
		std::lock_guard lk(_openMutex);
		return _open;
	}

	// ---------------------------------------------------------------------------------------------------------------
	bool Runtime::SetData(std::string_view menu, const std::string& key, DataValue value, std::string_view source)
	{
		const auto e = Find(menu);
		if (!e) {
			log::warn("set data '{}' on unknown menu '{}' ({}): ignored", key, menu, source);
			return false;
		}
		if (key.empty()) {
			log::warn("set data on '{}' ({}): empty key, ignored", e->def.name, source);
			return false;
		}
		std::string shown = std::visit(
			[](const auto& v) -> std::string {
				using T = std::decay_t<decltype(v)>;
				if constexpr (std::is_same_v<T, std::string>) return "\"" + v + "\"";
				else if constexpr (std::is_same_v<T, bool>) return v ? "true" : "false";
				else return std::to_string(v);
			},
			value);
		log::info("set data '{}'.{} = {} ({})", e->def.name, key, shown, source);
		data.Set(e->def.name, key, value);

		// deliver at once if the menu is open; the replay on the next open covers the closed case
		if (const auto ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(e->def.name)) {
			const std::string name = e->def.name;
			SKSE::GetTaskInterface()->AddUITask([name, key, value]() {
				if (const auto m = FindOpenMenu(name); m && m->IsOpenPhase()) m->DeliverMessage({ key, value });
			});
		}
		return true;
	}

	void Runtime::DeliverModEvent(const std::string& name, const std::string& strArg, float numArg)
	{
		for (auto* e : OpenMenus()) {
			const std::string menu = e->def.name;
			SKSE::GetTaskInterface()->AddUITask([menu, name, strArg, numArg]() {
				if (const auto m = FindOpenMenu(menu); m && m->IsOpenPhase()) m->DeliverModEvent(name, strArg, numArg);
			});
		}
	}

	void Runtime::SendModEvent(const std::string& name, const std::string& strArg, float numArg)
	{
		const auto src = SKSE::GetModCallbackEventSource();
		if (!src) {
			log::error("mod event '{}': the SKSE event source is missing", name);
			return;
		}
		SKSE::ModCallbackEvent ev{ RE::BSFixedString(name.c_str()), RE::BSFixedString(strArg.c_str()), numArg, nullptr };
		log::debug("send mod event '{}' strArg='{}' numArg={}", name, strArg, numArg);
		src->SendEvent(&ev);
	}

	void Runtime::BroadcastMenuState(const MenuEntry& e, bool opened)
	{
		log::info("menu '{}' {}", e.def.name, opened ? "opened" : "closed");
		SendModEvent(opened ? "Inkwell_MenuOpened" : "Inkwell_MenuClosed", e.def.name, 0.0f);
		BroadcastMenuMessage(e.def.name, opened);
	}

	void Runtime::ShowMessage(const std::string& text)
	{
		log::info("notification: {}", text);
		RE::SendHUDMessage::ShowHUDMessage(text.c_str());
	}

	void Runtime::ShowPendingNotification()
	{
		std::vector<std::string> notes;
		{
			std::lock_guard lk(_noteMutex);
			notes.swap(_pendingNotes);
		}
		for (const auto& n : notes) ShowMessage(n);
	}
}
