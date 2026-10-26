#include "Input.h"

#include "Menu.h"

namespace inkwell
{
	namespace
	{
		constexpr std::uint32_t kDikEscape = 1;

		// S7: the engine user events forwarded to On Input in 0.3 (exact strings are unverified in game).
		bool IsForwardedAction(std::string_view a)
		{
			for (const char* k : { "Up", "Down", "Left", "Right", "Accept", "Cancel", "Scroll Up", "Scroll Down", "PageUp", "PageDown" }) {
				if (a == k) return true;
			}
			return false;
		}

		class InputSink : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			static InputSink* Get()
			{
				static InputSink s;
				return &s;
			}

			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_events, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				if (!a_events) return RE::BSEventNotifyControl::kContinue;
				try {
					Handle(*a_events);
				} catch (const std::exception& e) {
					log::error("input sink: {}", e.what());
				} catch (...) {
					log::error("input sink: unknown exception");
				}
				return RE::BSEventNotifyControl::kContinue;
			}

		private:
			static void Handle(RE::InputEvent* first)
			{
				auto& rt = Runtime::Get();
				for (auto e = first; e; e = e->next) {
					// last input device, for inkwell.getInputDevice()
					const auto dev = e->GetDevice();
					if (dev == RE::INPUT_DEVICE::kGamepad) rt.lastDevice = InputDevice::kGamepad;
					else if (dev == RE::INPUT_DEVICE::kKeyboard || dev == RE::INPUT_DEVICE::kMouse) rt.lastDevice = InputDevice::kKeyboardMouse;

					const auto b = e->AsButtonEvent();
					if (!b || !b->IsDown()) continue;

					if (dev == RE::INPUT_DEVICE::kKeyboard) {
						const auto key = b->GetIDCode();
						log::trace("key down {}", key);
						if (key == kDikEscape) {
							if (auto* top = rt.TopmostOpen(); top && top->def.closeOnEscape) {
								log::info("Esc -> hide '{}'", top->def.name);
								rt.CloseMenu(top->def.name, "escape");
								continue;
							}
						}
						for (const auto& m : rt.Menus()) {
							if (!m->Active() || !m->eff.openKey || static_cast<std::uint32_t>(*m->eff.openKey) != key) continue;
							if (rt.IsMenuOpen(m->def.name)) {
								log::info("key {} -> hide '{}'", key, m->def.name);
								rt.CloseMenu(m->def.name, "hotkey");
							} else if (const auto why = rt.HotkeyBlockedReason(*m); !why.empty()) {
								log::info("key {} ignored for '{}': {}", key, m->def.name, why);
							} else {
								log::info("key {} -> show '{}'", key, m->def.name);
								rt.OpenMenu(m->def.name, "hotkey", false);
							}
						}
					}

					// On Input: the engine user event goes to the topmost open Inkwell menu only (S7)
					if (const auto* ue = b->QUserEvent().c_str(); ue && *ue && IsForwardedAction(ue)) {
						if (auto* top = rt.TopmostOpen()) {
							// Esc with closeOnEscape is handled above and never reaches a graph; Cancel is forwarded only when closeOnEscape is false
							if (std::string_view(ue) == "Cancel" && top->def.closeOnEscape) {
								if (dev == RE::INPUT_DEVICE::kGamepad) {
									log::info("gamepad Cancel -> hide '{}'", top->def.name);
									rt.CloseMenu(top->def.name, "gamepad cancel");
								}
								continue;
							}
							const std::string menu = top->def.name, action = ue;
							SKSE::GetTaskInterface()->AddUITask([menu, action]() {
								if (const auto m = FindOpenMenu(menu); m && m->IsOpenPhase()) m->DeliverInput(action);
							});
						}
					}
				}
			}
		};

		class ModEventSink : public RE::BSTEventSink<SKSE::ModCallbackEvent>
		{
		public:
			static ModEventSink* Get()
			{
				static ModEventSink s;
				return &s;
			}

			RE::BSEventNotifyControl ProcessEvent(const SKSE::ModCallbackEvent* a_event, RE::BSTEventSource<SKSE::ModCallbackEvent>*) override
			{
				if (!a_event) return RE::BSEventNotifyControl::kContinue;
				try {
					const std::string name = a_event->eventName.c_str() ? a_event->eventName.c_str() : "";
					const std::string strArg = a_event->strArg.c_str() ? a_event->strArg.c_str() : "";
					auto&             rt = Runtime::Get();
					// triggers.modEvent opens the menu; every event also goes to the open menus (On Mod Event)
					for (const auto& m : rt.Menus()) {
						if (m->Active() && !m->def.modEvent.empty() && LowerCopy(m->def.modEvent) == LowerCopy(name)) {
							log::info("mod event '{}' -> open '{}'", name, m->def.name);
							rt.OpenMenu(m->def.name, "modEvent", false);
						}
					}
					rt.DeliverModEvent(name, strArg, a_event->numArg);
				} catch (const std::exception& e) {
					log::error("mod event sink: {}", e.what());
				} catch (...) {
					log::error("mod event sink: unknown exception");
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void InstallInputSinks()
	{
		if (const auto dm = RE::BSInputDeviceManager::GetSingleton()) {
			dm->AddEventSink(InputSink::Get());
			log::info("input sink registered");
		} else {
			log::error("BSInputDeviceManager is null; hotkeys will not work");
		}
		if (const auto src = SKSE::GetModCallbackEventSource()) {
			src->AddEventSink(ModEventSink::Get());
			log::info("mod event sink registered");
		} else {
			log::error("SKSE mod event source is null; triggers.modEvent and On Mod Event will not work");
		}
	}
}
