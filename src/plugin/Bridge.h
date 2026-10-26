// The AS2 bridge: `_root.inkwell` (S4.2) installed through the GFx value API, and calls from C++ into the generated menu class (S7).
#pragma once
#include "Runtime.h"

#include <initializer_list>

namespace inkwell
{
	/// Creates `_root.inkwell` (and the legacy `_root.inkwellTrace` of the spike) in the movie. `ready` is set last.
	bool InstallBridge(RE::GFxMovieView& a_movie, MenuEntry& a_entry);

	/// True when the frame code has created `_root.inkwellMenu`.
	bool HasMenuClass(RE::GFxMovieView& a_movie);

	/// Invokes `_root.inkwellMenu.<member>(args)`. False (and a log line) when the object or call is missing.
	bool CallMenuClass(RE::GFxMovieView& a_movie, std::string_view a_menu, const char* a_member, std::initializer_list<RE::GFxValue> a_args);
}
