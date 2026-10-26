// Runtime state shared by the plugin's parts: the loaded menus, the open ones, config, tables. Engine-facing; the pure logic is in src/core.
#pragma once
#include "core/Ini.h"
#include "core/MenuConfig.h"
#include "core/Registry.h"
#include "core/Version.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace inkwell
{
	namespace log = SKSE::log;

	inline constexpr std::string_view kProduct = INKWELL_PRODUCT_NAME;  // the one product-name constant on the C++ side (set in CMakeLists.txt)

	enum class InputDevice { kKeyboardMouse, kGamepad };

	/// Runtime-wide settings from Inkwell.ini (S2).
	struct RuntimeConfig
	{
		std::string logLevel;  // as applied
		bool        devMode{ false };
		int         port{ 8788 };
		bool        watch{ true };
		bool        hasToken{ false };
	};

	/// One registered menu: the validated JSON, the resolved INI values and the slot it owns. Entries are never freed (the creator
	/// functions and the bridge hold raw pointers to them).
	struct MenuEntry
	{
		MenuDef                          def;
		EffectiveMenu                    eff;
		std::shared_ptr<const ParsedIni> modIni;  // the INI that holds this menu's section (or <modId>.ini); may be null
		std::string                      iniFile;
		std::string                      file;  // the JSON file it came from
		std::size_t                      slot{ 0 };
		std::atomic<bool>                registered{ false };  // UI::Register succeeded
		std::atomic<bool>                glueReady{ false };  // set by inkwell.glueReady(), reset when the menu is created
	};

	class Runtime
	{
	public:
		static Runtime& Get();

		// ---- load time ----
		/// Reads Data/SKSE/Plugins/Inkwell/Inkwell.ini and returns its problems for the caller to log.
		void LoadRuntimeConfig();
		/// Scans menus/*.json and the mod INIs (kDataLoaded).
		void LoadMenus();
		/// Registers every loaded menu with the engine (UI::Register).
		void RegisterMenus();

		// ---- lookup ----
		MenuEntry*                                    Find(std::string_view name) const;
		const std::vector<std::unique_ptr<MenuEntry>>& Menus() const { return _menus; }

		// ---- actions (thread-safe; only queue UI messages) ----
		bool OpenMenu(std::string_view name, std::string_view source, bool viaPapyrus);
		bool CloseMenu(std::string_view name, std::string_view source);
		bool IsMenuOpen(std::string_view name) const;
		/// Why a hotkey press must be ignored right now, or an empty string.
		std::string HotkeyBlockedReason(const MenuEntry& e) const;

		// ---- open menus ----
		void       OnMenuCreated(MenuEntry& e);
		void       OnMenuDestroyed(MenuEntry& e);
		MenuEntry* TopmostOpen() const;
		std::vector<MenuEntry*> OpenMenus() const;

		// ---- data and events ----
		/// Papyrus/messaging SetString/SetNumber/SetBool: keep it and deliver it to the menu if it is open.
		bool SetData(std::string_view menu, const std::string& key, DataValue value, std::string_view source);
		void DeliverModEvent(const std::string& name, const std::string& strArg, float numArg);
		void SendModEvent(const std::string& name, const std::string& strArg, float numArg);
		void BroadcastMenuState(const MenuEntry& e, bool opened);

		void ShowMessage(const std::string& text);
		void ShowPendingNotification();

		RuntimeConfig config;
		DataStore     data;
		FunctionTable functions;
		std::atomic<InputDevice> lastDevice{ InputDevice::kKeyboardMouse };

	private:
		std::vector<std::unique_ptr<MenuEntry>> _menus;  // index = template slot
		mutable std::mutex                      _openMutex;
		std::vector<MenuEntry*>                 _open;  // in the order they were created
		std::atomic<int>                        _refusedSlots{ 0 };
		std::vector<std::string>                _pendingNotes;
		std::mutex                              _noteMutex;
	};
}
