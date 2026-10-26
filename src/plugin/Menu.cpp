#include "Menu.h"

#include "Bridge.h"

#include <array>
#include <utility>

namespace inkwell
{
	namespace
	{
		constexpr long long kGlueTimeoutMs = 5000;  // S7: the runtime waits at most 5 s for glueReady() before it sends onOpen
	}

	InkwellMenu::InkwellMenu(MenuEntry& a_entry) :
		_entry(a_entry),
		_name(a_entry.def.name),
		_created(std::chrono::steady_clock::now())
	{
		const auto& d = a_entry.def;
		a_entry.glueReady = false;
		Runtime::Get().OnMenuCreated(a_entry);
		log::info("[{}] ctor: swf='{}' pausesGame={} cursor={} modal={} inputContext={} depth={}", _name, d.swf, d.pausesGame, d.cursor, d.modal,
			d.inputContext == InputContext::kMenuMode ? "menuMode" : "none", d.depth);

		using F = RE::UI_MENU_FLAGS;
		if (d.pausesGame) menuFlags.set(F::kPausesGame);
		if (d.cursor) menuFlags.set(F::kUsesCursor, F::kUpdateUsesCursor);
		if (d.modal) menuFlags.set(F::kModal);
		if (d.inputContext == InputContext::kMenuMode) {
			menuFlags.set(F::kUsesMenuContext);
			inputContext = Context::kMenuMode;
		}
		depthPriority = static_cast<std::int8_t>(d.depth);
		log::info("[{}] menuFlags=0x{:X} depthPriority={}", _name, menuFlags.underlying(), depthPriority);

		const auto sm = RE::BSScaleformManager::GetSingleton();
		if (!sm) {
			log::error("[{}] BSScaleformManager singleton is null", _name);
			_phase = Phase::kFailed;
			return;
		}
		// LoadMovieEx prepends "Interface/" and appends ".swf" itself, so it gets the normalized bare name (S1)
		log::info("[{}] LoadMovieEx name='{}'", _name, d.bareSwf);
		const bool ok = sm->LoadMovieEx(this, d.bareSwf, [this](RE::GFxMovieDef* a_def2) {
			log::info("[{}] LoadMovieEx callback: movieDef={} url='{}'", _name, static_cast<const void*>(a_def2), a_def2 ? a_def2->GetFileURL() : "");
		});
		log::info("[{}] LoadMovieEx returned {}; uiMovie={}", _name, ok, static_cast<const void*>(uiMovie.get()));
		if (!uiMovie) {
			log::error("[{}] no movie: '{}' is missing, misspelled (it is relative to Data/) or failed to load", _name, d.swf);
			_phase = Phase::kFailed;
			return;
		}

		RE::GFxValue root;
		log::info("[{}] _root exists: {}", _name, uiMovie->GetVariable(&root, "_root") && !root.IsUndefined());
		if (!InstallBridge(*uiMovie, a_entry)) log::error("[{}] the AS2 bridge could not be installed; the movie runs without it", _name);

		_phase = Phase::kWaitGlue;
		_created = std::chrono::steady_clock::now();
	}

	InkwellMenu::~InkwellMenu()
	{
		log::info("[{}] dtor (phase {})", _name, static_cast<int>(_phase));
		if (_openedBroadcast) {
			_openedBroadcast = false;
			Runtime::Get().BroadcastMenuState(_entry, false);
		}
		Runtime::Get().OnMenuDestroyed(_entry);
	}

	void InkwellMenu::PostCreate()
	{
		log::info("[{}] PostCreate", _name);
		RE::IMenu::PostCreate();
		if (_phase == Phase::kFailed) {
			// the movie could not be loaded: tell the player, then hide the (empty) menu again
			Runtime::Get().ShowMessage("Inkwell: menu '" + _name + "' could not load " + _entry.def.swf + ". See Inkwell.log.");
			if (const auto q = RE::UIMessageQueue::GetSingleton()) {
				log::warn("[{}] hiding the menu that failed to load", _name);
				q->AddMessage(_name, RE::UI_MESSAGE_TYPE::kHide, nullptr);
			}
		}
	}

	RE::UI_MESSAGE_RESULTS InkwellMenu::ProcessMessage(RE::UIMessage& a_message)
	{
		try {
			const auto type = a_message.type.get();
			if (type != RE::UI_MESSAGE_TYPE::kScaleformEvent && type != RE::UI_MESSAGE_TYPE::kUpdate) {
				log::debug("[{}] ProcessMessage type={}", _name, static_cast<int>(type));
			}
			if (type == RE::UI_MESSAGE_TYPE::kHide || type == RE::UI_MESSAGE_TYPE::kForceHide) BeginClose(type == RE::UI_MESSAGE_TYPE::kHide ? "kHide" : "kForceHide");
		} catch (const std::exception& e) {
			log::error("[{}] ProcessMessage: {}", _name, e.what());
		} catch (...) {
			log::error("[{}] ProcessMessage: unknown exception", _name);
		}
		return RE::IMenu::ProcessMessage(a_message);
	}

	void InkwellMenu::AdvanceMovie(float a_interval, std::uint32_t a_currentTime)
	{
		RE::IMenu::AdvanceMovie(a_interval, a_currentTime);
		if (_phase != Phase::kWaitGlue) return;
		try {
			const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - _created).count();
			const bool ready = _entry.glueReady.load();
			if (ready || waited >= kGlueTimeoutMs) BeginOpen(ready, waited);
		} catch (const std::exception& e) {
			log::error("[{}] AdvanceMovie: {}", _name, e.what());
			_phase = Phase::kFailed;
		} catch (...) {
			log::error("[{}] AdvanceMovie: unknown exception", _name);
			_phase = Phase::kFailed;
		}
	}

	void InkwellMenu::BeginOpen(bool a_glueReady, long long a_waitedMs)
	{
		if (a_glueReady) {
			log::info("[{}] glueReady() arrived after {} ms", _name, a_waitedMs);
		} else {
			log::warn("[{}] glueReady() did not arrive within {} ms (the SWF is not a generated menu or its start code failed); continuing", _name, a_waitedMs);
		}
		const bool hasClass = HasMenuClass(*uiMovie);
		log::info("[{}] _root.inkwellMenu exists: {}", _name, hasClass);

		// S7 order: replay stored data (On Runtime Message), then onOpen
		const auto stored = Runtime::Get().data.Snapshot(_name);
		log::info("[{}] replaying {} stored value(s)", _name, stored.size());
		for (const auto& s : stored) DeliverMessage(s);
		if (hasClass) CallMenuClass(*uiMovie, _name, "onOpen", {});
		_phase = Phase::kOpen;
		_openedBroadcast = true;
		Runtime::Get().BroadcastMenuState(_entry, true);
	}

	void InkwellMenu::BeginClose(const char* a_why)
	{
		if (_phase == Phase::kOpen) {
			log::info("[{}] closing ({}): calling onClose", _name, a_why);
			if (uiMovie && HasMenuClass(*uiMovie)) CallMenuClass(*uiMovie, _name, "onClose", {});
			_phase = Phase::kClosing;
			if (_openedBroadcast) {
				_openedBroadcast = false;
				Runtime::Get().BroadcastMenuState(_entry, false);
			}
		} else {
			log::info("[{}] closing ({}) in phase {}: no onClose (the menu never finished opening)", _name, a_why, static_cast<int>(_phase));
			_phase = Phase::kClosing;
		}
	}

	void InkwellMenu::DeliverMessage(const DataEntry& a_entry)
	{
		if (!uiMovie || !HasMenuClass(*uiMovie)) return;
		std::string text;
		double      number = 0;
		bool        flag = false;
		std::visit(
			[&](const auto& v) {
				using T = std::decay_t<decltype(v)>;
				if constexpr (std::is_same_v<T, std::string>) text = v;
				else if constexpr (std::is_same_v<T, bool>) flag = v;
				else number = v;
			},
			a_entry.value);
		log::debug("[{}] onMessage('{}')", _name, a_entry.key);
		CallMenuClass(*uiMovie, _name, "onMessage", { RE::GFxValue(a_entry.key.c_str()), RE::GFxValue(text.c_str()), RE::GFxValue(number), RE::GFxValue(flag) });
	}

	void InkwellMenu::DeliverModEvent(const std::string& a_name, const std::string& a_strArg, float a_numArg)
	{
		if (!uiMovie || !HasMenuClass(*uiMovie)) return;
		log::debug("[{}] onModEvent('{}')", _name, a_name);
		CallMenuClass(*uiMovie, _name, "onModEvent", { RE::GFxValue(a_name.c_str()), RE::GFxValue(a_strArg.c_str()), RE::GFxValue(static_cast<double>(a_numArg)) });
	}

	void InkwellMenu::DeliverInput(const std::string& a_action)
	{
		if (!uiMovie || !HasMenuClass(*uiMovie)) return;
		log::debug("[{}] onInput('{}')", _name, a_action);
		CallMenuClass(*uiMovie, _name, "onInput", { RE::GFxValue(a_action.c_str()) });
	}

	// ---- template slots ----------------------------------------------------------------------------------------------
	namespace
	{
		template <std::size_t I>
		RE::IMenu* CreateMenu()
		{
			const auto& menus = Runtime::Get().Menus();
			if (I >= menus.size()) {
				log::error("creator for slot {} invoked but no menu owns it", I);
				return nullptr;
			}
			log::info("[{}] creator invoked (slot {})", menus[I]->def.name, I);
			return new InkwellMenu(*menus[I]);
		}

		template <std::size_t... I>
		constexpr auto MakeCreators(std::index_sequence<I...>)
		{
			return std::array<RE::UI::Create_t*, sizeof...(I)>{ &CreateMenu<I>... };
		}

		const auto kCreators = MakeCreators(std::make_index_sequence<MENU_SLOTS>{});
	}

	bool RegisterSlot(std::size_t a_slot, const std::string& a_name)
	{
		if (a_slot >= kCreators.size()) return false;
		const auto ui = RE::UI::GetSingleton();
		if (!ui) return false;
		ui->Register(a_name, kCreators[a_slot]);
		return true;
	}

	InkwellMenu* FindOpenMenu(std::string_view a_name)
	{
		const auto e = Runtime::Get().Find(a_name);
		const auto ui = RE::UI::GetSingleton();
		if (!e || !ui) return nullptr;
		const auto m = ui->GetMenu(e->def.name);
		return m ? static_cast<InkwellMenu*>(m.get()) : nullptr;
	}
}
