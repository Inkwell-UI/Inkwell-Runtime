Scriptname Inkwell_API Hidden
{Inkwell Runtime API (S4.1). All functions are Global. Needs Inkwell.dll (the runtime) and SKSE; without the DLL these natives do nothing useful.
Menu names are the "name" of the menu JSON (case-insensitive). A call for an unknown menu is logged in Inkwell.log and returns false.
Mod events sent by the runtime: "Inkwell_MenuOpened" and "Inkwell_MenuClosed" (strArg = menu name, numArg = 0); use RegisterForModEvent.}

; Runtime version, packed as major*16777216 + minor*65536 + patch*16 (0.3.0 = 196608). Same packing as SKSE.GetPluginVersion("Inkwell").
Int Function GetVersion() Global Native

; Runtime version as text, for example "0.3.0".
String Function GetVersionString() Global Native

; Packs a version the same way GetVersion does, so a script can compare: GetVersion() >= MakeVersion(0, 3, 0). Plain Papyrus, works without the DLL.
Int Function MakeVersion(Int major, Int minor, Int patch) Global
	return major * 16777216 + minor * 65536 + patch * 16
EndFunction

; True when the runtime's dev mode is on (Inkwell.ini [Dev] DevMode=1). Always false in 0.3 phase 2 (dev mode is not built yet).
Bool Function IsDevMode() Global Native

; True when a menu JSON with this name loaded and was registered with the game.
Bool Function IsMenuRegistered(String menuName) Global Native

; True while the menu is open.
Bool Function IsMenuOpen(String menuName) Global Native

; Opens the menu. Returns true when it is open (or is being opened) after the call; false if the name is unknown or the menu JSON has triggers.papyrus = false.
; The call only queues the request; the menu appears a moment later on the game's UI thread.
Bool Function Open(String menuName) Global Native

; Closes the menu (On Close runs). True when it is closed (or is being closed) after the call.
Bool Function Close(String menuName) Global Native

; Send data to a menu. The runtime keeps the last value per (menu, key) until the game exits (not saved with the game), delivers it at once to an
; open menu as a runtime message (event "On Runtime Message", name = key) and replays all kept values, in the order they were set, at every open.
Function SetString(String menuName, String key, String value) Global Native
Function SetNumber(String menuName, String key, Float value) Global Native
Function SetBool(String menuName, String key, Bool value) Global Native
