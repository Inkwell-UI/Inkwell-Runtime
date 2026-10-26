#include "Runtime.h"

#include "Menu.h"
#include "Dev.h"
#include "Messaging.h"

#include <cctype>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
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
		config.token = ini.token.value_or("");
		log::info("log level '{}' ({}); Inkwell.ini [Dev] DevMode={} Port={} Watch={}", level, ini.logLevel ? "from Inkwell.ini" : "built-in default", config.devMode, config.port, config.watch);
		for (const auto& p : ini.problems) log::warn("{}:{} [{}] {}: {}", path.string(), p.line, p.section, p.key, p.message);
	}

	// ---------------------------------------------------------------------------------------------------------------
	namespace
	{
		void LogScan(const ScanResult& r)
		{
			for (const auto& m : r.messages) LogProblem(m.where, m.problem);
		}

		fs::path SwfOnDisk(const MenuDef& d) { return fs::path("Data") / d.swf; }

		// Dev hot reload loads a changed SWF under a name the engine has not cached. A file created while the game runs was not found
		// (LoadMovieEx returned false), so every name exists from startup: DEV_SLOTS copies per menu, each overwritten once and loaded once.
		constexpr int DEV_SLOTS = 16;
		std::string DevBare(const MenuEntry& e, int slot)
		{
			std::string key = MenuNameKey(e.def.name);
			for (auto& ch : key) if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
			return "Inkwell/_dev/" + key + "_" + std::to_string(slot);
		}
		bool DevCopy(const MenuEntry& e, int slot, std::error_code& ec)
		{
			const auto dest = fs::path("Data/Interface") / (DevBare(e, slot) + ".swf");
			fs::create_directories(dest.parent_path(), ec);
			if (ec) return false;
			fs::copy_file(SwfOnDisk(e.def), dest, fs::copy_options::overwrite_existing, ec);
			return !ec;
		}
	}

	std::unique_ptr<MenuEntry> Runtime::MakeEntry(MenuCandidate&& c, std::size_t slot)
	{
		auto e = std::make_unique<MenuEntry>();
		e->def = std::move(c.def);
		e->eff = c.eff;
		e->modIni = std::move(c.modIni);
		e->iniFile = std::move(c.iniFile);
		e->file = std::move(c.file);
		e->slot = slot;
		e->swfStamp = StampOf(SwfOnDisk(e->def));
		if (config.devMode) {
			std::error_code ec;
			for (int i = 0; i < DEV_SLOTS && DevCopy(*e, i, ec); ++i) {}
			if (ec) log::warn("dev mode: could not prepare the reload copies of '{}' ({})", e->def.name, ec.message());
		}
		return e;
	}

	void Runtime::LoadMenus()
	{
		std::error_code ec;
		log::info("scanning {} (cwd {})", kBaseDir.string(), fs::current_path(ec).string());
		auto scan = ScanMenus(kBaseDir);
		LogScan(scan);
		if (scan.dirMissing) {
			log::warn("menus directory not found: {} (no menus to register)", (kBaseDir / "menus").string());
			return;
		}

		int refused = 0;
		for (auto& c : scan.menus) {
			if (_menus.size() >= MENU_SLOTS) {
				++refused;
				log::error("slot exhaustion: {} not registered ({}: all {} template slots are in use)", c.def.name, c.file, MENU_SLOTS);
				continue;
			}
			auto e = MakeEntry(std::move(c), _menus.size());
			const auto& d2 = e->def;
			log::info("{}: loaded menu '{}' (slot {}) swf='{}' -> LoadMovieEx '{}'", e->file, d2.name, e->slot, d2.swf, d2.bareSwf);
			log::info("  flags: pausesGame={} cursor={} modal={} closeOnEscape={} inputContext={}{} depth={}", d2.pausesGame, d2.cursor, d2.modal, d2.closeOnEscape,
				d2.inputContext == InputContext::kMenuMode ? "menuMode" : "none", d2.inputContextExplicit ? "" : " (derived from modal)", d2.depth);
			log::info("  triggers: json hotkey={} papyrus={} modEvent='{}'; minRuntime {}", d2.hotkey ? std::to_string(*d2.hotkey) : "none", d2.papyrus, d2.modEvent, d2.minRuntime);
			log::info("  ini: {}; effective hotkey={} uiScale={} offset=({}, {}) theme='{}'", e->iniFile.empty() ? "none" : e->iniFile,
				e->eff.openKey ? std::to_string(*e->eff.openKey) : "none", e->eff.uiScale, e->eff.offsetX, e->eff.offsetY, e->eff.theme);
			std::lock_guard lk(_menusMutex);
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

		log::info("{} menu file(s) seen, {} loaded", scan.filesSeen, _menus.size());
		if (refused > 0) {
			_refusedSlots = refused;
			std::lock_guard lk(_noteMutex);
			_pendingNotes.push_back("Inkwell: " + std::to_string(refused) + (refused == 1 ? " menu was" : " menus were") + " not registered (limit " + std::to_string(MENU_SLOTS) + " menus). See Inkwell.log.");
		}
	}

	// ---- hot reload (S6) ---------------------------------------------------------------------------------------------
	MenuEntry* Runtime::FindAny(std::string_view name) const
	{
		const std::string key = MenuNameKey(name);
		std::lock_guard   lk(_menusMutex);
		for (const auto& e : _menus) {
			if (MenuNameKey(e->def.name) == key) return e.get();
		}
		return nullptr;
	}

	std::vector<MenuEntry*> Runtime::AllEntries() const
	{
		std::lock_guard         lk(_menusMutex);
		std::vector<MenuEntry*> v;
		for (const auto& m : _menus) v.push_back(m.get());
		return v;
	}

	Runtime::ReloadPlan Runtime::PlanReload(const std::string& only, bool force)
	{
		ReloadPlan plan;
		auto       scan = ScanMenus(kBaseDir);
		LogScan(scan);
		const std::string onlyKey = only.empty() ? std::string() : MenuNameKey(only);
		const auto        wanted = [&](const std::string& name) { return onlyKey.empty() || MenuNameKey(name) == onlyKey; };

		std::set<std::string> candidateKeys;
		bool                  mentioned = false;
		for (auto& c : scan.menus) {
			const std::string key = MenuNameKey(c.def.name);
			candidateKeys.insert(key);
			if (!wanted(c.def.name)) continue;
			mentioned = true;
			ReloadAction a;
			a.name = c.def.name;
			MenuEntry* e = FindAny(c.def.name);
			if (e && !e->removed) {
				const bool changed = !(e->def == c.def) || !(e->eff == c.eff) || e->file != c.file || e->swfStamp != StampOf(SwfOnDisk(c.def));
				if (!force && !changed) {
					plan.skipped.emplace_back(a.name, "unchanged");
					continue;
				}
				a.kind = ReloadAction::Kind::kUpdate;
			} else if (e) {
				a.kind = ReloadAction::Kind::kUpdate;  // was removed, its file is back: the slot is reused
			} else {
				a.kind = ReloadAction::Kind::kAdd;
			}
			a.candidate = std::move(c);
			plan.actions.push_back(std::move(a));
		}
		for (auto* e : AllEntries()) {
			if (e->removed || candidateKeys.count(MenuNameKey(e->def.name)) || !wanted(e->def.name)) continue;
			mentioned = true;
			if (scan.failedFiles.count(LowerCopy(e->file))) {
				plan.skipped.emplace_back(e->def.name, "its file is invalid now; the previous version stays loaded (see Inkwell.log)");
			} else {
				ReloadAction a;
				a.kind = ReloadAction::Kind::kRemove;
				a.name = e->def.name;
				plan.actions.push_back(std::move(a));
			}
		}
		plan.unknownMenu = !onlyKey.empty() && !mentioned;
		return plan;
	}

	void Runtime::ApplyUpdate(MenuEntry& e, MenuCandidate&& c)
	{
		const std::string keepName = e.def.name;  // the engine registered this spelling; other threads read it
		e.def = std::move(c.def);
		e.def.name = keepName;
		e.eff = c.eff;
		e.modIni = std::move(c.modIni);
		e.iniFile = std::move(c.iniFile);
		e.file = std::move(c.file);
		const auto stamp = StampOf(SwfOnDisk(e.def));
		if (config.devMode && stamp != e.swfStamp) {
			// The engine keeps a loaded movie by name (the same movieDef came back after a reload), so a changed SWF goes into a copy it has not loaded.
			std::error_code ec;
			if (e.devSlot >= DEV_SLOTS) {
				log::warn("hot reload: all {} reload copies of '{}' are used; restart the game to see further changes", DEV_SLOTS, e.def.name);
			} else if (DevCopy(e, e.devSlot, ec)) {
				e.loadName = DevBare(e, e.devSlot++);
			} else {
				log::warn("hot reload: could not copy the SWF ({}); the engine may show the old movie", ec.message());
			}
		}
		e.swfStamp = stamp;
		e.removed = false;
		log::info("hot reload: '{}' updated from {} (swf '{}' on disk: {} bytes, mtime tick {})", e.def.name, e.file, e.def.swf, e.swfStamp.size, e.swfStamp.mtime);
	}

	MenuEntry* Runtime::AddMenu(MenuCandidate&& c, std::string& why)
	{
		const auto ui = RE::UI::GetSingleton();
		if (!ui) {
			why = "UI singleton missing";
			return nullptr;
		}
		const RE::BSFixedString key(c.def.name.c_str());
		if (ui->menuMap.find(key) != ui->menuMap.end()) {
			why = "the engine already has a menu with this name (vanilla or another plugin)";
			return nullptr;
		}
		std::lock_guard lk(_menusMutex);
		if (_menus.size() >= MENU_SLOTS) {
			why = "no free template slot (" + std::to_string(MENU_SLOTS) + " in use; removed menus keep theirs)";
			return nullptr;
		}
		auto e = MakeEntry(std::move(c), _menus.size());
		if (!RegisterSlot(e->slot, e->def.name)) {
			why = "slot out of range";
			return nullptr;
		}
		e->registered = true;
		log::info("hot reload: new menu '{}' registered in slot {} ({})", e->def.name, e->slot, e->file);
		_menus.push_back(std::move(e));
		return _menus.back().get();
	}

	void Runtime::RemoveMenu(MenuEntry& e)
	{
		e.removed = true;
		log::info("hot reload: '{}' removed (its file is gone); slot {} stays reserved", e.def.name, e.slot);
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
		std::lock_guard   lk(_menusMutex);
		for (const auto& e : _menus) {
			if (e->Active() && MenuNameKey(e->def.name) == key) return e.get();
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
		devmode::OnMenuState(e, opened);
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
