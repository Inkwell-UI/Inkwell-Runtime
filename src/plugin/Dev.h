// Dev mode (S6): the debug WebSocket server, trace forwarding, live inspection and hot reload. Everything here is a no-op unless
// Inkwell.ini [Dev] DevMode=1 (never read from a mod INI).
#pragma once
#include "Runtime.h"

#include "core/DevProtocol.h"

namespace inkwell::devmode
{
	/// True when Inkwell.ini [Dev] DevMode=1 (Inkwell_API.IsDevMode, inkwell.isdevmode). Cheap, any thread.
	bool Active();

	/// Plugin load (single-threaded): installs the spdlog forwarder when dev mode is on, so no line is missed later.
	void InstallLogForwarder();

	/// kDataLoaded, after the menus are registered: banner, token, server, watcher. Never throws.
	void Start();

	// ---- hooks called from the rest of the runtime (all cheap no-ops when dev mode is off or no editor is connected) ----
	void OnMenuState(const MenuEntry& e, bool open);                  // any thread; from Runtime::BroadcastMenuState
	void OnMenuOpened(MenuEntry& e);                                  // UI thread, after onOpen: asks the menu for a variable dump if watched
	void Trace(std::string_view level, std::string_view menu, std::string_view node, std::string_view text);  // inkwell.log from AS2
	bool WantsWatch(std::string_view menu);
	bool WantsPins(std::string_view menu);
	void Watch(std::string_view menu, std::string_view graph, std::string_view scope, std::string_view name, const dev::Scalar& v);
	void Pin(std::string_view menu, std::string_view node, std::string_view pin, const dev::Scalar& v);
}
