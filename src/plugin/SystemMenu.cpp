#include "SystemMenu.h"

#include "Menu.h"

namespace inkwell
{
	namespace
	{
		// Path relative to Data/Interface. Source: flash/SystemMenuInjector.as (compiled by the editor's `yarn compile:system-injector`).
		constexpr const char* kHelperSwf = "Inkwell/SystemMenuInjector.swf";
		constexpr const char* kPickedEvent = "Inkwell_SystemMenu";  // sent by the helper, strArg = menu name
		constexpr double      kClipDepth = -8744;

		// "label<TAB>menu<LF>" per entry; read by the helper SWF from _global.InkwellSystemMenus. Tabs and line breaks in a label would break the format.
		std::string BuildEntries()
		{
			auto&       rt = Runtime::Get();
			std::string out;
			for (const auto& m : rt.Menus()) {
				if (!m->Active() || m->def.systemMenu.empty()) continue;
				auto label = m->def.systemMenu.front() == '$' ? rt.Translate(*m, m->def.systemMenu) : m->def.systemMenu;
				std::erase_if(label, [](char c) { return c == '\t' || c == '\n' || c == '\r'; });
				out += label + '\t' + m->def.name + '\n';
			}
			return out;
		}

		void Inject()
		{
			const auto entries = BuildEntries();
			if (entries.empty()) return;
			const auto ui = RE::UI::GetSingleton();
			const auto menu = ui ? ui->GetMenu(RE::JournalMenu::MENU_NAME) : nullptr;
			if (!menu || !menu->uiMovie) {
				log::warn("system menu: the Journal movie is not available, no entry added");
				return;
			}
			auto& mv = *menu->uiMovie;
			mv.SetVariable("_global.InkwellSystemMenus", RE::GFxValue(entries.c_str()));
			RE::GFxValue root, clip;
			if (!mv.GetVariable(&root, "_root") || root.IsUndefined()) {
				log::warn("system menu: Journal _root not found");
				return;
			}
			const RE::GFxValue mk[] = { RE::GFxValue("InkwellSystem"), RE::GFxValue(kClipDepth) };
			root.Invoke("createEmptyMovieClip", nullptr, mk, 2);
			if (!mv.GetVariable(&clip, "_root.InkwellSystem") || !clip.IsObject()) {
				log::warn("system menu: could not create the helper clip");
				return;
			}
			const RE::GFxValue load[] = { RE::GFxValue(kHelperSwf) };
			const bool         ok = clip.Invoke("loadMovie", nullptr, load, 1);
			log::info("system menu: loading {} into the Journal ({}):\n{}", kHelperSwf, ok, entries);
		}

		class JournalSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
		public:
			static JournalSink* Get()
			{
				static JournalSink s;
				return &s;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!a_event || !a_event->opening || a_event->menuName != RE::JournalMenu::MENU_NAME) return RE::BSEventNotifyControl::kContinue;
				try {
					Inject();
				} catch (const std::exception& e) {
					log::error("system menu inject: {}", e.what());
				} catch (...) {
					log::error("system menu inject: unknown exception");
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		class PickedSink : public RE::BSTEventSink<SKSE::ModCallbackEvent>
		{
		public:
			static PickedSink* Get()
			{
				static PickedSink s;
				return &s;
			}

			RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* a_event, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
			{
				if (!a_event || !a_event->eventName.c_str() || LowerCopy(a_event->eventName.c_str()) != LowerCopy(kPickedEvent)) return RE::BSEventNotifyControl::kContinue;
				try {
					const std::string name = a_event->strArg.c_str() ? a_event->strArg.c_str() : "";
					const auto        m = Runtime::Get().Find(name);
					if (!m || !m->Active() || m->def.systemMenu.empty()) {
						log::warn("system menu: '{}' picked but it has no triggers.systemMenu", name);
						return RE::BSEventNotifyControl::kContinue;
					}
					log::info("system menu: '{}' picked -> hide the Journal, open it", m->def.name);
					if (const auto q = RE::UIMessageQueue::GetSingleton()) q->AddMessage(RE::JournalMenu::MENU_NAME, RE::UI_MESSAGE_TYPE::kHide, nullptr);
					// open from a UI task: the queued kHide has been processed by then, so the Journal no longer owns the input
					const std::string menu = m->def.name;
					SKSE::GetTaskInterface()->AddUITask([menu]() {
						const auto ui = RE::UI::GetSingleton();
						log::info("system menu: opening '{}', Journal still open: {}", menu, ui && ui->IsMenuOpen(RE::JournalMenu::MENU_NAME));
						Runtime::Get().OpenMenu(menu, "systemMenu", false);
					});
				} catch (const std::exception& e) {
					log::error("system menu pick: {}", e.what());
				} catch (...) {
					log::error("system menu pick: unknown exception");
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void InstallSystemMenuSinks()
	{
		if (const auto ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(JournalSink::Get());
			log::info("system menu: Journal sink registered");
		} else {
			log::error("UI singleton is null; triggers.systemMenu will not work");
		}
		if (const auto src = SKSE::GetModCallbackEventSource()) src->AddEventSink(PickedSink::Get());
		else log::error("SKSE mod event source is null; triggers.systemMenu will not work");
	}
}
