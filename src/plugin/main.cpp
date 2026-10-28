// Inkwell Runtime: SKSE plugin entry. Registers the menus defined by Data/SKSE/Plugins/Inkwell/menus/*.json (S1), applies the user INIs (S2),
// and exposes the Papyrus API (S4.1), the AS2 bridge (S4.2) and the SKSE messaging interface (S4.3). Everything logs verbosely.
#include "Dev.h"
#include "Input.h"
#include "SystemMenu.h"
#include "Menu.h"
#include "Messaging.h"
#include "Papyrus.h"
#include "Runtime.h"
#include "Saved.h"

namespace
{
	using namespace inkwell;

	void OnDataLoaded()
	{
		log::info("kDataLoaded");
		auto& rt = Runtime::Get();
		try {
			rt.LoadMenus();
			rt.RegisterMenus();
			InstallInputSinks();
			InstallSystemMenuSinks();
			devmode::Start();
		} catch (const std::exception& e) {
			log::error("kDataLoaded: {}", e.what());
		} catch (...) {
			log::error("kDataLoaded: unknown exception");
		}
	}

	void OnSKSEMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostLoad:
			log::info("kPostLoad");
			break;
		case SKSE::MessagingInterface::kPostPostLoad:
			log::info("kPostPostLoad");
			DispatchInterfaceReady();
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			OnDataLoaded();
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			log::info("{}", a_msg->type == SKSE::MessagingInterface::kNewGame ? "kNewGame" : "kPostLoadGame");
			Runtime::Get().ShowPendingNotification();
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	// SKSE::Init opens <log_directory>/<plugin name>.log, i.e. Documents/My Games/Skyrim Special Edition/SKSE/Inkwell.log.
	// It starts at trace so nothing is lost; Inkwell.ini [Runtime] LogLevel (default INKWELL_DEFAULT_LOG_LEVEL) is applied right after.
	SKSE::Init(a_skse, { .logLevel = spdlog::level::trace });
	log::info("{} Runtime {} (spec {}, protocol {}) on Skyrim {}, SKSE {}", kProduct, ToString(kRuntimeVersion), kSpecVersion, kRuntimeProtocol,
		REL::Module::get().version().string(), a_skse->RuntimeVersion().string());

	try {
		Runtime::Get().LoadRuntimeConfig();
		devmode::InstallLogForwarder();
		InitMessaging();
		InitSavedState();
		const bool papyrus = SKSE::GetPapyrusInterface()->Register(RegisterPapyrus);
		log::info("Papyrus registration callback queued: {}", papyrus);
		SKSE::GetMessagingInterface()->RegisterListener(OnSKSEMessage);
	} catch (const std::exception& e) {
		log::error("plugin load: {}", e.what());
		return false;
	}
	return true;
}
