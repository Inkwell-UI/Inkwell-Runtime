#pragma once
#include "Runtime.h"

namespace inkwell
{
	/// Registers the listener for other plugins' messages (kGetInterface). Call at plugin load.
	void InitMessaging();
	/// Broadcasts kInterfaceReady with the interface pointer (kPostPostLoad).
	void DispatchInterfaceReady();
	/// Broadcasts kMenuOpened / kMenuClosed (data = menu name).
	void BroadcastMenuMessage(const std::string& a_menu, bool a_opened);
}
