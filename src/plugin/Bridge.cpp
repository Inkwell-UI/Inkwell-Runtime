#include "Bridge.h"

#include "Menu.h"

#include <array>

namespace inkwell
{
	namespace
	{
		enum class Fn
		{
			kLog,
			kGlueReady,
			kOpenMenu,
			kCloseMenu,
			kSendModEvent,
			kCallRuntime,
			kPlaySound,
			kGetSetting,
			kGetInputDevice,
			kGetSaved,
			kSetSaved,
			kWatch,
			kPin,
			kLegacyTrace,
			kCount
		};

		std::string Str(const RE::GFxFunctionHandler::Params& p, std::uint32_t i)
		{
			if (i >= p.argCount) return {};
			const auto& a = p.args[i];
			if (a.IsString()) return a.GetString();
			if (a.IsNumber()) {
				const double n = a.GetNumber();
				return n == static_cast<long long>(n) ? std::to_string(static_cast<long long>(n)) : std::to_string(n);
			}
			if (a.IsBool()) return a.GetBool() ? "true" : "false";
			return {};
		}

		double Num(const RE::GFxFunctionHandler::Params& p, std::uint32_t i)
		{
			return (i < p.argCount && p.args[i].IsNumber()) ? p.args[i].GetNumber() : 0.0;
		}

		void RetBool(const RE::GFxFunctionHandler::Params& p, bool b)
		{
			if (p.retVal) p.retVal->SetBoolean(b);
		}

		void RetString(const RE::GFxFunctionHandler::Params& p, const std::string& s)
		{
			if (p.retVal && p.movie) p.movie->CreateString(p.retVal, s.c_str());
		}

		spdlog::level::level_enum LevelOf(const std::string& l)
		{
			if (l == "trace") return spdlog::level::trace;
			if (l == "debug") return spdlog::level::debug;
			if (l == "warn") return spdlog::level::warn;
			if (l == "error") return spdlog::level::err;
			return spdlog::level::info;
		}

		const char* kNames[] = { "log", "glueReady", "openMenu", "closeMenu", "sendModEvent", "callRuntime", "playSound", "getSetting", "getInputDevice", "getSaved", "setSaved", "watch", "pin", "inkwellTrace" };

		class BridgeFn : public RE::GFxFunctionHandler
		{
		public:
			explicit BridgeFn(Fn a_kind) :
				_kind(a_kind) {}

			void Call(Params& p) override
			{
				// Never throw into the engine: a bad AS2 call must only log.
				try {
					auto* e = static_cast<MenuEntry*>(p.userData);
					if (!e) {
						log::error("[AS2] bridge call {} without menu context", kNames[static_cast<int>(_kind)]);
						return;
					}
					Dispatch(p, *e);
				} catch (const std::exception& ex) {
					log::error("[AS2] bridge call {} failed: {}", kNames[static_cast<int>(_kind)], ex.what());
				} catch (...) {
					log::error("[AS2] bridge call {} failed: unknown exception", kNames[static_cast<int>(_kind)]);
				}
			}

		private:
			void Dispatch(Params& p, MenuEntry& e)
			{
				auto& rt = Runtime::Get();
				const std::string& menu = e.def.name;
				switch (_kind) {
				case Fn::kLog: {
					const auto level = Str(p, 0), node = Str(p, 1), text = Str(p, 2);
					// goes to spdlog as "[AS2] [<menu>] [<nodeId>] text"; the dev stream (S6) arrives with the dev server
					spdlog::default_logger()->log(LevelOf(level), "[AS2] [{}] [{}] {}", menu, node, text);
					break;
				}
				case Fn::kGlueReady:
					log::info("[AS2] [{}] glueReady()", menu);
					e.glueReady = true;
					break;
				case Fn::kOpenMenu: {
					const auto name = Str(p, 0);
					RetBool(p, rt.OpenMenu(name, "as2:" + menu, false));
					break;
				}
				case Fn::kCloseMenu: {
					auto name = Str(p, 0);
					if (name.empty()) name = menu;
					RetBool(p, rt.CloseMenu(name, "as2:" + menu));
					break;
				}
				case Fn::kSendModEvent: {
					const auto ev = Str(p, 0);
					if (ev.empty()) {
						log::warn("[AS2] [{}] sendModEvent: empty event name ignored", menu);
						break;
					}
					rt.SendModEvent(ev, Str(p, 1), static_cast<float>(Num(p, 2)));
					break;
				}
				case Fn::kCallRuntime: {
					const auto name = Str(p, 0);
					const auto r = rt.functions.Call(name, Str(p, 1));
					if (!r) log::warn("[AS2] [{}] callRuntime('{}'): unknown function, returning \"\"", menu, name);
					else log::debug("[AS2] [{}] callRuntime('{}') -> '{}'", menu, name, *r);
					RetString(p, r.value_or(std::string{}));
					break;
				}
				case Fn::kPlaySound: {
					const auto id = Str(p, 0);
					bool       played = false;
					if (auto* form = RE::TESForm::LookupByEditorID(id)) {
						if (auto* desc = form->As<RE::BGSSoundDescriptorForm>()) {
							RE::UIUtils::PlayMenuSound(desc);
							played = true;
						}
					}
					log::info("[AS2] [{}] playSound('{}') -> {}", menu, id, played ? "played" : "not found (no sound descriptor with that editor ID)");
					RetBool(p, played);
					break;
				}
				case Fn::kGetSetting: {
					const auto section = Str(p, 0), key = Str(p, 1), dflt = Str(p, 2);
					std::string value = dflt;
					bool        found = false;
					if (e.modIni) {
						if (const auto s = e.modIni->raw.find(LowerCopy(section)); s != e.modIni->raw.end()) {
							if (const auto k = s->second.find(LowerCopy(key)); k != s->second.end()) {
								value = k->second;
								found = true;
							}
						}
					}
					log::debug("[AS2] [{}] getSetting('{}', '{}', '{}') -> '{}'{}", menu, section, key, dflt, value, found ? "" : " (default)");
					RetString(p, value);
					break;
				}
				case Fn::kGetInputDevice:
					RetString(p, rt.lastDevice.load() == InputDevice::kGamepad ? "gamepad" : "keyboardMouse");
					break;
				case Fn::kGetSaved:
					// Saved scope (S5) is a later phase: the stored value is always absent, so the default comes back.
					log::debug("[AS2] [{}] getSaved('{}'): saved state is not implemented in this build; returning the default", menu, Str(p, 0));
					if (p.retVal && p.argCount >= 2) *p.retVal = p.args[1];
					break;
				case Fn::kSetSaved:
					log::debug("[AS2] [{}] setSaved('{}'): saved state is not implemented in this build; ignored", menu, Str(p, 0));
					break;
				case Fn::kWatch:
				case Fn::kPin:
					break;  // dev mode only; no-ops otherwise (S6)
				case Fn::kLegacyTrace: {
					std::string text;
					for (std::uint32_t i = 0; i < p.argCount; ++i) text += (i ? ", " : "") + Str(p, i);
					log::info("[AS2] [{}] inkwellTrace({})", menu, text);
					break;
				}
				default: break;
				}
			}

			Fn _kind;
		};

		RE::GPtr<BridgeFn>& Handler(Fn k)
		{
			static std::array<RE::GPtr<BridgeFn>, static_cast<std::size_t>(Fn::kCount)> handlers;
			auto&                                                                        h = handlers[static_cast<std::size_t>(k)];
			if (!h) h = RE::GPtr<BridgeFn>{ new BridgeFn(k) };
			return h;
		}
	}

	bool InstallBridge(RE::GFxMovieView& mv, MenuEntry& e)
	{
		const auto& menu = e.def.name;
		RE::GFxValue obj;
		mv.CreateObject(&obj);
		if (!obj.IsObject()) {
			log::error("[{}] bridge: CreateObject did not return an object", menu);
			return false;
		}

		bool ok = true;
		const auto member = [&](const char* name, const RE::GFxValue& v) {
			const bool r = obj.SetMember(name, v);
			if (!r) log::error("[{}] bridge: SetMember('{}') failed", menu, name);
			ok = ok && r;
		};
		const auto str = [&](const char* name, const std::string& s) {
			RE::GFxValue v;
			mv.CreateString(&v, s.c_str());
			member(name, v);
		};

		member("ready", RE::GFxValue(false));
		member("version", RE::GFxValue(static_cast<double>(Pack(kRuntimeVersion))));
		str("menu", menu);
		member("uiScale", RE::GFxValue(e.eff.uiScale));
		member("offsetX", RE::GFxValue(static_cast<double>(e.eff.offsetX)));
		member("offsetY", RE::GFxValue(static_cast<double>(e.eff.offsetY)));
		str("theme", e.eff.theme);

		for (int i = 0; i < static_cast<int>(Fn::kLegacyTrace); ++i) {
			RE::GFxValue fn;
			mv.CreateFunction(&fn, Handler(static_cast<Fn>(i)).get(), &e);
			member(kNames[i], fn);
		}
		log::info("[{}] bridge: object with {} members built (ok={})", menu, static_cast<int>(Fn::kLegacyTrace) + 7, ok);

		// attach, then flip `ready` last (S4.2)
		const bool attached = mv.SetVariable("_root.inkwell", obj);
		log::info("[{}] installed _root.inkwell: {}", menu, attached);
		if (!attached) return false;

		// the spike's single function, kept so the spike SWFs (which call _root.inkwellTrace) still show a line in the log
		{
			RE::GFxValue fn;
			mv.CreateFunction(&fn, Handler(Fn::kLegacyTrace).get(), &e);
			log::info("[{}] installed legacy _root.inkwellTrace: {}", menu, mv.SetVariable("_root.inkwellTrace", fn));
		}

		const bool ready = mv.SetVariable("_root.inkwell.ready", RE::GFxValue(true));
		RE::GFxValue back, ver, name;
		mv.GetVariable(&back, "_root.inkwell.ready");
		mv.GetVariable(&ver, "_root.inkwell.version");
		mv.GetVariable(&name, "_root.inkwell.menu");
		log::info("[{}] bridge ready: set={} read back ready={} version={} menu='{}'", menu, ready, back.IsBool() && back.GetBool(),
			ver.IsNumber() ? static_cast<std::uint64_t>(ver.GetNumber()) : 0, name.IsString() ? name.GetString() : "<not a string>");
		return ok && ready;
	}

	bool HasMenuClass(RE::GFxMovieView& mv)
	{
		RE::GFxValue v;
		return mv.GetVariable(&v, "_root.inkwellMenu") && v.IsObject();
	}

	bool CallMenuClass(RE::GFxMovieView& mv, std::string_view menu, const char* member, std::initializer_list<RE::GFxValue> args)
	{
		if (!HasMenuClass(mv)) {
			log::warn("[{}] _root.inkwellMenu does not exist; {} not called", menu, member);
			return false;
		}
		const std::string          path = std::string("_root.inkwellMenu.") + member;
		const std::vector<RE::GFxValue> a(args);
		RE::GFxValue               result;
		const bool                 ok = mv.Invoke(path.c_str(), &result, a.empty() ? nullptr : a.data(), static_cast<std::uint32_t>(a.size()));
		log::debug("[{}] {}({} arg(s)) -> {}", menu, path, a.size(), ok);
		if (!ok) log::warn("[{}] {} failed (the member may not exist)", menu, path);
		return ok;
	}
}
