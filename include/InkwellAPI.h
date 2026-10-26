/*
 * InkwellAPI.h - the C interface of the Inkwell Runtime for other SKSE plugins.
 *
 * Licence: MIT (this header only). Copy it into your plugin freely. The runtime DLL itself (Inkwell.dll) is GPL-3.0-or-later
 * because it links CommonLibSSE-NG; this header contains no GPL code, only the binary interface (struct layouts, message numbers).
 *
 * Copyright (c) 2026 the Inkwell authors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software
 * is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
 * IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * How to get the interface (SKSE messaging, plugin name "Inkwell"):
 *   1. Inkwell broadcasts INKWELL_MSG_INTERFACE_READY (1) at kPostPostLoad; message data = const InkwellInterfaceV1*.
 *   2. Or send INKWELL_MSG_GET_INTERFACE (2, no data) to "Inkwell" after kPostPostLoad; Inkwell answers with
 *      INKWELL_MSG_INTERFACE_READY addressed to you.
 *   3. Inkwell broadcasts INKWELL_MSG_MENU_OPENED (100) and INKWELL_MSG_MENU_CLOSED (101); data = const char* menu name,
 *      valid only during the call.
 * Fields of InkwellInterfaceV1 are only ever appended; check `size` before you read a field that is not in your copy of the header.
 * Message numbers and struct layouts are API once public and never change within a major version.
 */
#pragma once

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#define INKWELL_PLUGIN_NAME "Inkwell"
#define INKWELL_INTERFACE_VERSION 1u

/* message types (S4.3) */
#define INKWELL_MSG_INTERFACE_READY 1u
#define INKWELL_MSG_GET_INTERFACE 2u
#define INKWELL_MSG_MENU_OPENED 100u
#define INKWELL_MSG_MENU_CLOSED 101u

#ifdef __cplusplus
namespace Inkwell
{
	enum : uint32_t
	{
		kInterfaceReady = INKWELL_MSG_INTERFACE_READY,
		kGetInterface = INKWELL_MSG_GET_INTERFACE,
		kMenuOpened = INKWELL_MSG_MENU_OPENED,
		kMenuClosed = INKWELL_MSG_MENU_CLOSED,
	};
}
#endif

/*
 * A function for the "Call Runtime" node. `arg` is the node's argument string; write a NUL-terminated result into `out`
 * (at most `outSize` bytes including the NUL; longer results are cut). `ctx` is the pointer passed to RegisterFunction.
 * Called on the game's UI thread: be quick and do not block.
 */
typedef void (*InkwellFunction)(const char* arg, char* out, uint32_t outSize, void* ctx);

typedef struct InkwellInterfaceV1
{
	uint32_t size;              /* sizeof(InkwellInterfaceV1) of the runtime that built it */
	uint32_t interfaceVersion;  /* 1 */
	uint32_t runtimeVersion;    /* packed: major*16777216 + minor*65536 + patch*16 (same as SKSE plugin versions) */

	/* menu names are the "name" of the menu JSON; they are case-insensitive. Open/Close only queue a UI message and return true when queued. */
	bool (*OpenMenu)(const char* menu);
	bool (*CloseMenu)(const char* menu);
	bool (*IsMenuOpen)(const char* menu);

	/* Same as Inkwell_API.SetString/SetNumber/SetBool: keep the last value per (menu, key), replay it at every open, deliver it at once
	   to an open menu (the "On Runtime Message" event). */
	void (*SetString)(const char* menu, const char* key, const char* value);
	void (*SetNumber)(const char* menu, const char* key, double value);
	void (*SetBool)(const char* menu, const char* key, bool value);

	/* Adds `name` to the Call Runtime function table. Names are `plugin.function`, lower-case, ^[a-z0-9_.]{3,64}$; a duplicate or invalid
	   name is refused (returns false). Registered functions live until the game exits. */
	bool (*RegisterFunction)(const char* name, InkwellFunction fn, void* ctx);
} InkwellInterfaceV1;
