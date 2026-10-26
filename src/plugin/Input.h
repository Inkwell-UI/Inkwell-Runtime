#pragma once

namespace inkwell
{
	/// Hotkeys, Esc, user-event forwarding (On Input) and mod-event sinks. Call once at kDataLoaded, after the menus registered.
	void InstallInputSinks();
}
