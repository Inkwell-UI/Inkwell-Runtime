// Inkwell runtime spike: register + open ONE custom Scaleform menu defined by JSON.
// Everything logs verbosely; in-game testing is manual.
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <fstream>
#include <thread>

namespace
{
	namespace log = SKSE::log;
	using json = nlohmann::json;

	constexpr std::string_view kProduct = INKWELL_PRODUCT_NAME;
	constexpr std::uint32_t    kDikEscape = 1;
	constexpr std::size_t      kMaxMenus = 8;  // Register() takes a context-free fn pointer, so one template slot each

	struct MenuDef
	{
		std::string   name;
		std::string   swf;
		bool          pausesGame{ false };
		bool          cursor{ false };
		bool          modal{ false };
		std::uint32_t hotkey{ 0 };  // DIK scancode, 0 = none
		int           depth{ 3 };
		int           autoOpenSeconds{ 0 };  // spike fallback: open this long after a save loads, 0 = off
	};

	std::vector<MenuDef> g_defs;

	// Called from AS2 as _root.inkwellTrace(...) to prove the movie runs and can reach C++.
	class TraceFn : public RE::GFxFunctionHandler
	{
	public:
		void Call(Params& a_params) override
		{
			std::string text;
			for (std::uint32_t i = 0; i < a_params.argCount; ++i) {
				const auto& a = a_params.args[i];
				if (i) text += ", ";
				if (a.IsString()) text += a.GetString();
				else if (a.IsNumber()) text += std::to_string(a.GetNumber());
				else if (a.IsBool()) text += a.GetBool() ? "true" : "false";
				else text += "<type " + std::to_string(static_cast<int>(a.GetType())) + ">";
			}
			log::info("[AS2] inkwellTrace({})", text);
		}
	};

	class InkwellMenu : public RE::IMenu
	{
	public:
		explicit InkwellMenu(const MenuDef& a_def) :
			_name(a_def.name)
		{
			log::info("[{}] ctor: swf='{}' pausesGame={} cursor={} modal={} depth={}", _name, a_def.swf, a_def.pausesGame, a_def.cursor, a_def.modal, a_def.depth);

			using F = RE::UI_MENU_FLAGS;
			if (a_def.pausesGame) menuFlags.set(F::kPausesGame);
			if (a_def.cursor) menuFlags.set(F::kUsesCursor, F::kUpdateUsesCursor);
			if (a_def.modal) {
				menuFlags.set(F::kModal, F::kUsesMenuContext);
				inputContext = Context::kMenuMode;
			}
			depthPriority = static_cast<std::int8_t>(a_def.depth);
			log::info("[{}] menuFlags=0x{:X} depthPriority={}", _name, menuFlags.underlying(), depthPriority);

			const auto sm = RE::BSScaleformManager::GetSingleton();
			if (!sm) {
				log::error("[{}] BSScaleformManager singleton is null", _name);
				return;
			}
			const bool ok = sm->LoadMovieEx(this, a_def.swf, [this](RE::GFxMovieDef* a_def2) {
				log::info("[{}] LoadMovieEx callback: movieDef={} url='{}'", _name, static_cast<const void*>(a_def2), a_def2 ? a_def2->GetFileURL() : "");
			});
			log::info("[{}] LoadMovieEx returned {}; uiMovie={}", _name, ok, static_cast<const void*>(uiMovie.get()));
			if (!uiMovie) {
				log::error("[{}] no movie: SWF missing, wrong path ('{}' is relative to Data/), or load failed", _name, a_def.swf);
				return;
			}

			RE::GFxValue root;
			log::info("[{}] _root exists: {}", _name, uiMovie->GetVariable(&root, "_root") && !root.IsUndefined());

			static RE::GPtr<TraceFn> handler{ new TraceFn() };
			RE::GFxValue             fn;
			uiMovie->CreateFunction(&fn, handler.get());
			log::info("[{}] installed _root.inkwellTrace: {}", _name, uiMovie->SetVariable("_root.inkwellTrace", fn));
		}

		~InkwellMenu() override { log::info("[{}] dtor", _name); }

		void PostCreate() override
		{
			log::info("[{}] PostCreate", _name);
			RE::IMenu::PostCreate();
		}

		RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& a_message) override
		{
			if (a_message.type != RE::UI_MESSAGE_TYPE::kScaleformEvent && a_message.type != RE::UI_MESSAGE_TYPE::kUpdate) {
				log::info("[{}] ProcessMessage type={}", _name, static_cast<int>(a_message.type.get()));
			}
			return RE::IMenu::ProcessMessage(a_message);
		}

	private:
		std::string _name;
	};

	template <std::size_t I>
	RE::IMenu* CreateMenu()
	{
		log::info("[{}] creator invoked (slot {})", g_defs[I].name, I);
		return new InkwellMenu(g_defs[I]);
	}

	template <std::size_t... I>
	constexpr auto MakeCreators(std::index_sequence<I...>)
	{
		return std::array<RE::UI::Create_t*, sizeof...(I)>{ &CreateMenu<I>... };
	}

	void LoadDefs()
	{
		const std::filesystem::path dir = std::filesystem::path("Data/SKSE/Plugins") / kProduct / "menus";
		log::info("scanning {} (cwd {})", dir.string(), std::filesystem::current_path().string());
		std::error_code ec;
		if (!std::filesystem::is_directory(dir, ec)) {
			log::warn("menus directory not found: {}", dir.string());
			return;
		}
		for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
			if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
			const auto file = entry.path().string();
			try {
				std::ifstream in(entry.path());
				const auto    j = json::parse(in);
				MenuDef       d;
				d.name = j.at("name").get<std::string>();
				d.swf = j.at("swf").get<std::string>();
				if (d.name.empty() || d.swf.empty()) throw std::runtime_error("empty name or swf");
				if (const auto f = j.find("flags"); f != j.end()) {
					d.pausesGame = f->value("pausesGame", false);
					d.cursor = f->value("cursor", false);
					d.modal = f->value("modal", false);
				}
				if (const auto t = j.find("trigger"); t != j.end()) { d.hotkey = t->value("hotkey", 0u); d.autoOpenSeconds = t->value("autoOpenSeconds", 0); }
				d.depth = j.value("depth", 3);  // spec lists depth as a menu field; optional here
				if (g_defs.size() >= kMaxMenus) {
					log::warn("{}: skipped, spike supports {} menus", file, kMaxMenus);
					continue;
				}
				log::info("{}: loaded menu '{}' swf='{}' hotkey={}", file, d.name, d.swf, d.hotkey);
				g_defs.push_back(std::move(d));
			} catch (const std::exception& e) {
				log::error("{}: skipped, {}", file, e.what());
			}
		}
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
			const auto ui = RE::UI::GetSingleton();
			const auto q = RE::UIMessageQueue::GetSingleton();
			if (!ui || !q) return RE::BSEventNotifyControl::kContinue;

			for (auto e = *a_events; e; e = e->next) {
				const auto b = e->AsButtonEvent();
				if (!b || b->GetDevice() != RE::INPUT_DEVICE::kKeyboard || !b->IsDown()) continue;
				const auto key = b->GetIDCode();
				for (const auto& d : g_defs) {
					const bool open = ui->IsMenuOpen(d.name);
					if (open && (key == d.hotkey || key == kDikEscape)) {
						log::info("key {} -> hide '{}'", key, d.name);
						q->AddMessage(d.name, RE::UI_MESSAGE_TYPE::kHide, nullptr);
					} else if (!open && d.hotkey && key == d.hotkey) {
						if (ui->IsMenuOpen(RE::Console::MENU_NAME) || ui->IsMenuOpen(RE::MainMenu::MENU_NAME)) {
							log::info("key {} ignored: console or main menu open", key);
							continue;
						}
						log::info("key {} -> show '{}'", key, d.name);
						q->AddMessage(d.name, RE::UI_MESSAGE_TYPE::kShow, nullptr);
					}
				}
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	void OnDataLoaded()
	{
		log::info("kDataLoaded");
		LoadDefs();

		static const auto creators = MakeCreators(std::make_index_sequence<kMaxMenus>{});
		const auto        ui = RE::UI::GetSingleton();
		if (!ui) {
			log::error("UI singleton is null; nothing registered");
			return;
		}
		for (std::size_t i = 0; i < g_defs.size(); ++i) {
			ui->Register(g_defs[i].name, creators[i]);
			log::info("UI::Register('{}') done", g_defs[i].name);
		}

		if (const auto dm = RE::BSInputDeviceManager::GetSingleton()) {
			dm->AddEventSink(InputSink::Get());
			log::info("input sink registered");
		} else {
			log::error("BSInputDeviceManager is null; hotkeys will not work");
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	// SKSE::Init opens <log_directory>/<plugin name>.log, i.e. Documents/My Games/Skyrim Special Edition/SKSE/Inkwell.log
	SKSE::Init(a_skse, { .logLevel = spdlog::level::trace });
	log::info("{} spike loaded; Skyrim runtime {}, SKSE {}", kProduct, REL::Module::get().version().string(), a_skse->RuntimeVersion().string());

	SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* a_msg) {
		if (a_msg->type == SKSE::MessagingInterface::kDataLoaded) OnDataLoaded();
	});
	return true;
}
