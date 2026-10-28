#pragma once

namespace inkwell
{
	/// triggers.systemMenu: adds an entry per such menu to the Journal's System page (helper SWF patches the page) and opens the menu when it is picked.
	/// Call once at kDataLoaded, after the menus registered.
	void InstallSystemMenuSinks();
}
