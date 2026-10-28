# In-game test notes: Inkwell Runtime 0.3 (runtime core, saved state, events, dev mode)

This is the detailed reference (log lines per step). **The owner's step-by-step test in the order to run it is `TEST-IN-GAME-0.3.md` in the workspace root (`D:\Mods\Inkwell`); start there.** Claude cannot run Skyrim, so nothing below has been seen in game yet. Each step says what you should see; the lines in `Inkwell.log` are the evidence. Please send back `Inkwell.log`, `skse64.log` and the answers to the "Report" questions at the end.

Tested game so far for the spikes: Skyrim SE 1.7.104 (Steam). AE and GOG are unverified (decision 3).

## 0. What you need

| What | Where it comes from |
|---|---|
| `Inkwell.dll` | the CI artifact `Inkwell-dll` of the workflow `runtime` (GitHub, Actions, latest run on `main`), or `build\Release\Inkwell.dll` after a local build |
| Address Library for SKSE Plugins (v13 or newer for 1.7.104) | already installed for the spikes |
| `InkwellSpikeA.swf` ... `InkwellSpikeD.swf` | the MO2 mod `Inkwell Spikes` from the 0.0 spikes (`Interface/`). Keep it enabled. They are the SWFs of sections 1 to 8 (the editor-built samples are in `TEST-IN-GAME-0.3.md`). They do not call `glueReady()`, so each open waits 5 s for it and logs a warning: that is expected here |
| `Inkwell_API.pex` | compile `papyrus\Source\Scripts\Inkwell_API.psc` (russo: `papyrus.exe compile -nocache -i papyrus\Source\Scripts -o <out>`); CI does not compile it |
| `InkwellTestHelper.pex` (optional) | the same for `test-assets\papyrus\Source\Scripts\InkwellTestHelper.psc` with `-h papyrus\stubs`; only usable from an attached script, see section 3 |

Make a new MO2 mod folder `Inkwell Runtime Test` (never write into the game's Data) with this content:

```
SKSE/Plugins/Inkwell.dll
SKSE/Plugins/Inkwell/menus/InkwellTest_A.json ... InkwellTest_D.json, InkwellTest_MissingSwf.json   (from test-assets/menus/)
SKSE/Plugins/Inkwell/InkwellTest.ini                                                                   (from test-assets/ini/)
Scripts/Inkwell_API.pex, Scripts/InkwellTestHelper.pex
Source/Scripts/Inkwell_API.psc
```

Do **not** copy `Inkwell.ini` yet (step 7). The log is `trace` level by default in this version, so you get everything.

Hotkeys of the test menus (DirectInput codes): **O** = A (24), **P** = B (25, moved to **U** (22) by the INI), **L** = C (38, removed by the INI), **K** = D (37), **N** = MissingSwf (49).

## 1. The DLL loads (main menu)

1. Start the game with `skse64_loader.exe`, reach the main menu, quit.
2. `skse64.log` must say `plugin Inkwell.dll ... loaded correctly`. If it says the plugin needs a newer/older Address Library or is incompatible, send it.
3. `Inkwell.log` starts with:
   - `Inkwell Runtime 0.4.0 (spec 7, protocol 1) on Skyrim 1-7-104-0, SKSE ...`
   - `no Data\SKSE\Plugins\Inkwell\Inkwell.ini (that is fine): built-in defaults`
   - `log level 'trace' (built-in default) ...`
   - `messaging: listener for other plugins registered: true` (if `false`, tell me)
   - `Papyrus registration callback queued: true`
   - `kPostLoad`, `kPostPostLoad`, `messaging: kInterfaceReady broadcast (interface v1, size ...): true`
   - `kDataLoaded`, `scanning Data\SKSE\Plugins\Inkwell ...`
   - `...InkwellTest.ini: 3 menu section(s)` plus warnings for the deliberate bad lines of that file: `[Menu.InkwellTest_C] UIScale: clamped 9.000000 to 2.000000`, `[Menu.InkwellTest_A] OffsetX: 5000 is outside -960..960`
   - for every menu: `loaded menu 'InkwellTest_A' (slot 0) swf='Interface/InkwellSpikeA.swf' -> LoadMovieEx 'InkwellSpikeA'`, then `flags: ...`, `triggers: ...`, `ini: ...; effective hotkey=...`. `InkwellTest_B` shows `effective hotkey=22 uiScale=1.5 offset=(40, -20) theme='dark'`; `InkwellTest_C` shows `effective hotkey=none uiScale=2`
   - `5 menu file(s) seen, 5 loaded`, then `UI::Register('InkwellTest_A') done (slot 0)` for each, `input sink registered`, `mod event sink registered`.

## 2. Open and close with the hotkey (in game, no console commands)

Load a save. Do not open the console yet. Everything in this section works from the keyboard only.

| Press | Expected on screen | Expected in `Inkwell.log` |
|---|---|---|
| **O** | menu A opens (SpikeA content); game paused, cursor shown | `key 24 -> show 'InkwellTest_A'`, `open 'InkwellTest_A' (hotkey): queue kShow`, `creator invoked (slot 0)`, `ctor: ... pausesGame=true cursor=true modal=true inputContext=menuMode depth=3`, `menuFlags=0x41D depthPriority=3`, `LoadMovieEx name='InkwellSpikeA'`, `LoadMovieEx returned true`, `_root exists: true`, `installed _root.inkwell: true`, `bridge ready: set=true read back ready=true version=196608 menu='InkwellTest_A'`, `PostCreate`. Then, because the spike SWF has no glue: `glueReady() did not arrive within 5000 ms ...` after about 5 s, `_root.inkwellMenu exists: false`, `replaying 0 stored value(s)`, `menu 'InkwellTest_A' opened` |
| **O** again | menu closes | `key 24 -> hide`, `close ... queue kHide`, `closing (kHide)`, `dtor`, `menu 'InkwellTest_A' closed` |
| **O**, then **Esc** | menu closes (closeOnEscape=true). Note whether the pause menu also opens | `Esc -> hide 'InkwellTest_A'` |
| **P** (or **U**, the INI moved it) | menu B opens with the cursor but the game keeps running | `key 22 -> show 'InkwellTest_B'`; pressing **P** now only logs `key 25` at trace level (no menu) |
| **L** | nothing (the INI removed C's hotkey) | `key down 38` at trace level, no `show` |
| **K**, then **Esc** | D opens (game paused). **Esc must NOT close it** (closeOnEscape=false). Press **K** to close it | `key 1 ...` not followed by `hide`; if the SWF had On Input handlers they would get `Cancel` |
| **N** | nothing visible; a notification `Inkwell: menu 'InkwellTest_MissingSwf' could not load Interface/Inkwell/Test/DoesNotExist.swf. See Inkwell.log.` | `LoadMovieEx returned false` (or a null movie), `no movie: ...`, `hiding the menu that failed to load`. **Unverified**: this is the case most likely to misbehave (an IMenu without a movie). Tell me if the game froze or crashed |

Then open the console (`~`) and press **O**: nothing opens; the log says `key 24 ignored for 'InkwellTest_A': the console is open`. Also try the hotkey with the inventory or journal open (expected `ignored ...: an application menu ... is open` or `the game is paused by a menu`), and while another Inkwell menu is open.

For every flag combination note what you saw (this is the O6 matrix; the editor will rely on it): A = pausesGame+cursor+modal+menuMode, B = cursor only, C = nothing (needs its INI block removed, see section 3), D = pausesGame only without Esc. Questions: did the game pause? was the cursor visible? could you still move the character (WASD) while the menu was open? did a click reach the world?

## 3. Papyrus (not testable without an attached script)

This Skyrim SE has no console command to call a script function, and a Papyrus function can only be called from a script that is attached to something (a quest, an alias, a magic effect). `InkwellTestHelper` and `Inkwell_API` are `Hidden` global-function scripts: nothing in this repo attaches them, and the helper cannot register for keys (`RegisterForKey` needs a `Quest`/`Form` script object). The Editor's generated runtime-check ESP (`<modId>_RuntimeCheck.esp`, no master, one script on a player alias) only does the version check and never calls `Inkwell_API`; the widget ESP is for HUD widgets. So nothing in this repo or the Editor can call the API for you:

| Check | Without an attached script |
|---|---|
| Open and close a menu with a hotkey, Esc, flags, INI overrides, bad files, slot limit (sections 1, 2, 5, 6, 7) | **Doable**, no Papyrus |
| The AS2 bridge (section 4) and the editor slice (`TEST-IN-GAME-0.3-slice.md`) | **Doable**, hotkey from the menu JSON |
| `Inkwell_API.Open/Close/IsMenuOpen/IsMenuRegistered/SetString/SetNumber/SetBool/GetVersion` | **Not doable.** Needs a script. The C++ side is covered only by the `registered functions` log line at kDataLoaded |
| `triggers.papyrus=false` refusal (menu D), unknown menu name | **Not doable** (Papyrus call) |
| Mod event open (`triggers.modEvent`) and `Inkwell_MenuOpened/Closed` receivers | **Not doable** (needs `SendModEvent`, a script). Another installed mod that sends a mod event could trigger it, but none is known to |
| Menu C (its hotkey is removed by the INI) | Doable by editing the INI: delete the `[Menu.InkwellTest_C]` block of `InkwellTest.ini` and C opens with **L** |

What you can still read without a script: the `Inkwell_MenuOpened`/`Inkwell_MenuClosed` lines in `Inkwell.log` (`send mod event ... strArg='InkwellTest_A'`) on every open and close of a hotkey menu, which shows the runtime sends them.

**If you want the Papyrus tests anyway** (optional, needs the Creation Kit): make a plugin with one start-game-enabled quest, attach a script `extends Quest` that calls `RegisterForKey(<scan code>)` in `OnInit`/`OnPlayerLoadGame` and, in `OnKeyDown`, calls `Inkwell_API.Open("InkwellTest_C")`, `Inkwell_API.Open("InkwellTest_D")` (must be refused), `Inkwell_API.Open("NoSuchMenu")`, `Inkwell_API.Close(...)`, `Inkwell_API.SetString("InkwellTest_A", "greeting", "hello")`, and `Debug.Notification(Inkwell_API.GetVersionString())`. Compile with `D:\Mod Tools\Russo Compiler\papyrus.exe compile -nocache -i <src> -o <out>`. Expected log lines: `open 'InkwellTest_C' (papyrus): queue kShow`, `refused, the menu JSON has triggers.papyrus=false`, `open 'NoSuchMenu' (papyrus): unknown menu`, `set data 'InkwellTest_A'.greeting = "hello" (papyrus)`, and `mod event 'InkwellTest_Open' -> open 'InkwellTest_A'` for `SendModEvent`. `InkwellTestHelper.SendOpenEvent` / `ReportVersion` can be called from that script. This is not done by Claude and not verified.

## 4. The AS2 bridge

The spike SWFs call `_root.inkwellTrace("hello")` in frame 1. The runtime keeps that function as a legacy alias, so a line `[AS2] [InkwellTest_A] inkwellTrace(...)` proves a C++ function installed through the GFx value API is callable from the movie. The generated menu SWFs of phase 3/4 will use `_root.inkwell.log(level, nodeId, text)` (`[AS2] [<menu>] [<nodeId>] text`) and `glueReady()`; until then the bridge object is verified only from the C++ side (the `bridge ready: ... read back ready=true version=196608 menu='...'` line). If the read-back says `ready=false`, or `SetMember(...) failed` appears, send the log: that is the "many native members through CreateObject/SetMember" point marked unverified in S4.2.

## 5. Bad files do not crash

Copy the files of `test-assets/menus-bad/` into `SKSE/Plugins/Inkwell/menus/` one by one (or all at once), start the game and read the log. Every file must be skipped with one `error` line naming the file and the field, and the other menus must still load. Expected `error` text per file:

| File | Expected |
|---|---|
| `bad_01_syntax.json` | `skipped, not valid JSON` |
| `bad_02_spike_form.json` | `skipped, specVersion: required` |
| `bad_03_name.json` | `skipped, name: must match ^[A-Za-z][A-Za-z0-9_]{1,63}$` |
| `bad_04_swf_path.json` | `skipped, swf: must be relative to Data/, start with Interface/ ...` |
| `bad_05_hotkey.json` | `skipped, triggers.hotkey: expected a DirectInput scan code from 2 to 255 (1 is Escape)` |
| `bad_06_needs_newer_runtime.json` | `skipped, ERROR menu 'InkwellBad_NewerRuntime' needs Inkwell Runtime 0.5.0, this is 0.4.0: not registered` |
| `bad_07_other_major.json` | the same text with `1.0.0` |
| `zz_08_duplicate_name.json` | `skipped, menu name 'inkwelltest_a' is already used by ...InkwellTest_A.json` |
| `bad_09_engine_name.json` | loads (valid JSON) but then `skipped, the engine already has a menu with this name (vanilla or another plugin)` at `UI::Register` time. It takes a template slot |
| `bad_10_depth.json` | `skipped, depth: expected a whole number from 0 to 15` |

Slot limit: also copy `test-assets/menus-slots/` (14 tiny menus, files sort after the test menus). Expected: `slot exhaustion: InkwellFill_NN not registered (...: all 16 template slots are in use)` for the surplus ones, and, after you load a save, one notification `Inkwell: N menus were not registered (limit 16 menus). See Inkwell.log.`

## 6. INI overrides

Edit `InkwellTest.ini` (the file in your MO2 mod), change `OpenKey` of `[Menu.InkwellTest_B]`, restart, and check `ini: ...; effective hotkey=` in the log and the new key in game. Delete the whole file: the JSON hotkeys apply again (P opens B). An invalid value only produces a `warn` line with the file, line number and key.

## 7. Runtime-wide Inkwell.ini

Copy `test-assets/ini/Inkwell.ini` to `SKSE/Plugins/Inkwell/Inkwell.ini`. Expected: `log level 'debug' (from Inkwell.ini)` (trace lines disappear). Setting `DevMode=1` turns dev mode on (section 12). A `[Dev] or `[Runtime]` section in `InkwellTest.ini` is ignored with a warning (a mod cannot switch dev mode on).

## 8. What I could not check (please look)

- Everything above, in game. The flag mapping (`pausesGame`, `cursor`, `modal`, `inputContext` to engine flags) is the spike's guess plus one change: `kUsesMenuContext` and `inputContext = menuMode` now follow the JSON `inputContext` (default: menuMode when `modal`), not `modal` itself.
- The hotkey is ignored while `IsApplicationMenuOpen()`, `IsModalMenuOpen()` or `GameIsPaused()` is true. If the hotkey never opens a menu in plain gameplay, the log line `ignored ...: <reason>` tells which test fires.
- `RegisterListener(nullptr, ...)` (listen to messages from every plugin, used for `kGetInterface`). If SKSE rejects a null sender the log says `listener ... registered: false`; the broadcast `kInterfaceReady` still works.
- Sub-path SWF names (`Interface/Inkwell/<modId>/<Swf>.swf` giving `LoadMovieEx 'Inkwell/<modId>/<Swf>'`, O2): the missing-SWF menu only proves failure handling; a real sub-path SWF is test T1 of `TEST-IN-GAME-0.3.md`.
- AE and GOG.

## 9. Saved state (S5, phase 5)

All steps are log-driven. They need a menu SWF that calls `inkwell.getSaved` / `inkwell.setSaved`: build one in the Editor with a `Saved` variable (menu graph, Set Variable / Toggle / Counter on that variable) and deploy it with a menu JSON. The spike SWFs never call them. Nothing here has been seen in game. Use a variable `counter` (Number, default 0) in a menu `Foo`.

| Step | Expected in `Inkwell.log` |
|---|---|
| Plugin load | `co-save: callbacks registered (unique id 0x494E4B57, record SVAR v1)` |
| New game (or first load of a save without Inkwell data), open `Foo` | `co-save: revert, saved variables cleared`, then `getSaved('counter'): nothing stored, default`, the menu shows 0 |
| Click the counter to 3, close the menu | `setSaved('counter') stored (written to the co-save at the next game save)` for each change |
| Save the game (any save, quick or manual) | `co-save: wrote SVAR v1 for 'foo': 1 variable(s), NN bytes`, `co-save: saved 1 menu record(s), 0 failed`. The record is in `<save name>.skse` next to the `.ess` |
| Load that save (from the main menu or in game) | `co-save: revert, saved variables cleared`, `co-save: loaded 'foo': 1 variable(s)`, `co-save: load done, 1 menu record(s) loaded, 0 skipped`. Open `Foo`: `getSaved('counter') -> 3`, the menu shows 3 |
| Load an older save made before the counter existed (or start a new game) | `revert`, no `loaded` line, `nothing stored, default`: the menu shows 0 again. A value from the previous session must **not** leak into it |
| Change the counter to 5 but do not save, load the save | the menu shows 3 (the unsaved value is gone) |
| Uninstall `Inkwell.dll`, load, save, reinstall, load that new save | the game loads without errors; the saved value is gone (SKSE drops the records without the plugin); with the DLL back the menu shows 0 |
| Optional: change the variable's type in the editor (Number to String), redeploy, open `Foo` after loading the save with 3 | `getSaved('counter'): stored value has type 1 but the default has type 2; returning the default` (warn) |

Not covered in game: a record with a higher version (the load callback logs `SVAR record version N is not understood ... skipped` and carries on; the byte-level cases are unit-tested). Stale values of variables a newer build no longer declares stay in the record (O8). Papyrus cannot read saved variables in 0.3.

## 10. Mod events

`sendModEvent` from a menu graph and `onModEvent` into an open menu need no Papyrus and no console.

1. Build a menu `Sender` in the Editor whose graph has a button with the **Send Mod Event** node (name `InkwellTest_Open`, strArg `hi`, numArg `7`), hotkey on a free key.
2. Open `Sender` and press the button. Expected: `[AS2] ... sendModEvent` is not logged at info; look for `send mod event 'InkwellTest_Open' strArg='hi' numArg=7` (debug), then the runtime's own sink: `mod event 'InkwellTest_Open' -> open 'InkwellTest_A'`, and menu A opens (its JSON has `triggers.modEvent`). That shows the event went out on the SKSE mod event source and reached a receiver.
3. A menu with an **On Mod Event** node for `InkwellTest_Open` that is open when the event is sent: `[Foo] onModEvent('InkwellTest_Open')` (debug) and the node's output (a text change). The sender itself receives its own event, so `Sender` can show it too. Events sent while a menu is closed are dropped: no `onModEvent` line for it.
4. `Inkwell_MenuOpened` / `Inkwell_MenuClosed` are sent on every open and close (`send mod event 'Inkwell_MenuOpened' strArg='Foo' numArg=0`), so an open menu with an On Mod Event node for them fires for *other* menus opening.
5. `Inkwell_API.SetString/SetNumber/SetBool` and `RegisterForModEvent` receivers need an attached quest script (see section 3); not tested here.

Runtime messages (`onMessage`) come from the same sources as the replay in section 2: `replaying N stored value(s)` and `[Foo] onMessage('<key>')`.

## 11. getSetting, getInputDevice, callRuntime

- `getSetting(section, key, default)` (S4.2 has three arguments) reads the mod INI that holds the menu's section, any section, keys case-insensitive, and always returns a String (the generated code converts). `InkwellTest.ini` has `[Custom]` with `Greeting=hello from the ini`, `Count=3`, `Enabled=true`. From a Get Setting node: `getSetting('Custom', 'Greeting', 'none') -> 'hello from the ini'`; a missing key or section logs `... -> 'none' (default)`; a menu whose mod has no INI always gets the default. Edit the INI, reopen the menu, the new value appears (the INI is read at game start, so restart the game; the hot reload of section 12 re-reads the menu JSON and the INIs but does not refresh what `getSetting` reads from a menu that is already loaded: reopen it).
- `getInputDevice()` returns `keyboardMouse` or `gamepad` from the last input event. Log: none by default (the call is not logged); the node's output is the evidence. Press a gamepad button, reopen or re-trigger, expect `gamepad`.
- `callRuntime(name, arg)` (one argument, S4.2): `inkwell.version` -> `0.3.0`, `inkwell.ismenuopen` with a menu name -> `1`/`0`, `inkwell.isdevmode` -> `0` (`1` in dev mode); anything else logs `callRuntime('x'): unknown function, returning ""` (warn) and returns an empty string. Functions registered by other SKSE plugins (`RegisterFunction`) need such a plugin; none exists yet.

## 12. Dev mode: debug server, hot reload, live inspection (S6, phase 6)

Nothing here has been seen in game. The server, the codec, the limits and the file watcher are tested on this machine without the game (`ctest --preset tests`, test `dev`, and `scripts/dev-client.mjs` against `InkwellDevTests --serve`); what is **not** seen is the game side: closing and reopening a real menu, the movie cache, `inkwell.watch`/`inkwell.pin` from a real SWF.

### 12.1 What dev mode is, and the off switch

Dev mode exists only if `Data/SKSE/Plugins/Inkwell/Inkwell.ini` has `[Dev]` and `DevMode=1`. A `[Dev]` section in a mod INI is ignored with a warning. Without it: no socket, no file watcher, no token, `Inkwell_API.IsDevMode()` is false, `inkwell.watch`/`inkwell.pin` do nothing, the log says `dev mode is off`.

```ini
; Data/SKSE/Plugins/Inkwell/Inkwell.ini (in MO2: your mod folder, or the overwrite folder)
[Dev]
DevMode=1
; Port=8788      (default; the Figma bridge uses 8787)
; Watch=1        (file watcher; default 1)
; Token=         (leave empty: the runtime makes one and writes it here)
```

Start the game once to the main menu, then check `Inkwell.log`:

| Expect | Meaning |
|---|---|
| a block of `=====` lines with `INKWELL DEV MODE IS ON` | the gate is open (warn level, on purpose) |
| `dev mode: generated a new token and wrote it to Data\SKSE\Plugins\Inkwell\Inkwell.ini` (only the first time) | the token now sits in `Inkwell.ini` under `[Dev] Token=` (64 hex digits). With MO2 the file can appear in the `overwrite` folder |
| `dev server on 127.0.0.1:8788, token ab12...ef90` | listening; the log shows the token **masked**, never in full |
| `file watcher on the menu JSON, INIs and SWFs: running (300 ms debounce)` | hot reload on file changes |
| `dev server not started: cannot listen on ... (port in use?)` | change `[Dev] Port`; the game itself is not affected |

`Inkwell_API.IsDevMode()` (or Call Runtime `inkwell.isdevmode`) now returns true / `1`. Remove `DevMode=1` (or the file) for a release setup.

### 12.2 Check the server without the editor (`scripts/dev-client.mjs`)

Needs Node 22 or newer only (no packages). With the game running (main menu is enough, Alt-Tab out):

```powershell
cd Inkwell-Runtime
node scripts/dev-client.mjs --ini "<path to Inkwell.ini>" --seconds 30
# or: node scripts/dev-client.mjs --token <64 hex digits> --port 8788
```

Expected output: `HTTP/1.1 101 Switching Protocols`, then `helloAck ok=true runtime 0.3.0 ... protocol 1 spec 6 game ...` with one `menu <name> <swf> open=false` line per registered menu. Server lines (`trace [...] runtime: ...`) scroll by: every spdlog line, so many at `trace` log level. Then:

| Command | Expected |
|---|---|
| `... --subscribe InkwellTest_A --watch --pins` | no error; `Inkwell.log`: `dev: subscribe InkwellTest_A watch=1 pins=1`. Open the menu in game: a `menuState` line `open:true`, close it: `open:false` |
| the same with `--subscribe NoSuchMenu` | an `error` message with `"code":"unknown-menu"`, the connection stays open |
| `... --reload` (all menus) | `reloadAck` with `"ok":true` and every menu in `reloaded`. Log: `hot reload (editor request): reloaded N ...` |
| `... --reload InkwellTest_A` while A is open | the menu closes and opens again by itself (see 12.4); `menuState` false then true; `reloadAck` lists `InkwellTest_A` |
| `... --reload NoSuchMenu` | `reloadAck` with `"ok":false` and an `error` with `"code":"unknown-menu"` |
| a wrong token | `helloAck ok=false error=unauthorized`, `server closed: code 1008`. After 5 wrong tokens within a minute the server refuses connections for 60 s |
| `... --origin http://x --expect-fail` and `... --host evil.example --expect-fail` | `HTTP/1.1 403 Forbidden`, exit code 0 |
| two clients at once | the second one gets an `error` with `"code":"busy"` and is closed; the first keeps running |

### 12.3 Trace and watch (needs a menu SWF built for it)

Menu SWFs from the Editor call `inkwell.log(level, nodeId, text)`; Log nodes and the try/catch failure lines arrive as `trace [info] as2 <menu> node=<id>: text`. A deliberate error (a node that throws) shows up as `trace [error] ... node=<id>` and the menu keeps working. `inkwell.watch` and `inkwell.pin` calls are emitted only into builds deployed with dev mode on; with `--subscribe M --watch` the client prints `watch ...` lines (a full dump when you subscribe and again every time M opens), with `--pins` it prints `pin ...` lines at most 20 per second per pin. The spike SWFs never call any of this.

### 12.4 Hot reload

1. Open a menu (hotkey), keep it open. Run `node scripts/dev-client.mjs --ini ... --reload <menu> --seconds 5`.
2. Expected log: `hot reload: closing '<menu>' to apply the new files`, the menu's own `closing (kHide): calling onClose` and `dtor`, then `hot reload: '<menu>' updated from ... (swf '...' on disk: N bytes, mtime tick T)`, then `open '<menu>' (hot reload): queue kShow` and a new `LoadMovieEx` line. The menu closes and opens again within a second.
3. **The movie cache is not verified.** Replace the menu's SWF with a visibly different one (or just the Editor's Deploy), run the reload, and look at the menu: new content = the engine re-read the file. If the old content shows, tell me: the fallback is a distinct SWF name per deploy (S6). The log line with the file size and mtime proves what was on disk at that moment.
4. File watcher: with the game running, change a value in the menu JSON (for example `depth`) or copy another SWF over the menu's SWF. After about 300 ms of quiet: `hot reload (file watcher): reloaded 1, ...` and the menu (if open) reopens. A broken JSON (delete a brace): `skipped ... its file is invalid now; the previous version stays loaded`, nothing breaks; fix the file, it reloads.
5. Slots: copy a menu JSON to a new name with a new menu `name` while the game runs: `hot reload: new menu '<name>' registered in slot N`, its hotkey works. With all 16 slots used: `skipped ... no free template slot`. Delete a menu JSON: `hot reload: '<name>' removed ... slot N stays reserved`, its hotkey stops, and the slot is **not** reused by a different name (the engine keeps the old name); the same name coming back reuses it.

### 12.5 With the Editor as the client

1. Settings, game folder: the MO2 mod folder (or Data folder) the Editor deploys to. Tick **Dev deploy** in the Deploy menu; the Editor writes `[Dev] DevMode=1` and a `Token=` of 64 hex digits into `Inkwell.ini` there.
2. Start Skyrim; check the log lines of 12.1. The Game dot in the status bar turns green after the Editor connects; hover shows the runtime version.
3. Deploy: files are written, then the Editor sends `reload`. The open menu reloads; the Trace & Watch panel shows lines with node ids.
4. Close the game: the dot turns grey.

Report what differs from 12.2 to 12.5, and any `error`/`warn` line in `Inkwell.log` containing `[dev]` or `hot reload`.

## 13. Translations (0.4.0, SPEC-CHANGES V13.2)

Skyrim loads `Interface/Translations/<Name>_<LANGUAGE>.txt` only for loaded plugins (spike S-B, `TEST-IN-GAME-0.4.md` section 6), so the runtime reads the files of a menu itself. **Not yet run in game** (the in-game test is `TEST-IN-GAME-0.4.md` S-B2).

- **Which file:** the menu JSON field `translations` (a mod name, `^[A-Za-z][A-Za-z0-9_]{0,63}$`; absent = no translations) and the game language (Skyrim.ini `[General] sLanguage`, upper case, `ENGLISH` if it is missing or not a language name). `Data/Interface/Translations/<translations>_<LANGUAGE>.txt`, read once on first use and kept for the session. No fallback language in the runtime: the SWF carries the source text and keeps it when the runtime has no answer; the Editor writes a file for every language, with the source text where a translation is missing.
- **Format** (the Spec's `parseTranslationFile`; shared vectors in `test-vectors/translations/`, both readers must agree): BOM decides the encoding (FF FE = UTF-16 LE, FE FF = UTF-16 BE, EF BB BF or none = UTF-8). Lines end CRLF, LF or CR; blank lines and lines starting `;` or `#` are skipped; `$KEY<TAB>text`, the first TAB splits; the two characters backslash-n are a line break, a doubled backslash is one backslash. A bad line or a duplicate key is logged as a warning (`Demo_FRENCH.txt: line 5: no TAB between key and text`) and skipped; the later duplicate wins. Keys are matched ignoring case.
- **Bridge:** `inkwell.language` (string) and `inkwell.translate(key)` (the text, or the key itself when there is none). Log lines: `game language GERMAN (translations are read from Data\Interface\Translations on first use)` at start, `loaded <file>: N entries, UTF-16 LE` (debug) on first use, `[AS2] [<menu>] translate('$Key') -> 'text'` (trace).
- **Unit tests:** `InkwellTests` (translation vectors, the store with real files, case-insensitive keys, missing files, bad names, odd bytes, the `translations` menu field).

## Report

Please send `Inkwell.log`, `skse64.log`, and: (1) did the DLL load; (2) per menu A to D: paused, cursor, movement, Esc behaviour; (3) which hotkeys were ignored and why; (4) did the missing-SWF menu crash, hang or only notify; (5) any `error` or `warn` line you did not expect.

## System menu entry (0.5.1, triggers.systemMenu)

Set `"systemMenu": "Test Menu"` in a menu JSON and copy `Interface/Inkwell/SystemMenuInjector.swf` to `Data/Interface/Inkwell/`. Open the Journal > System: the entry must sit under Load, Save/Load/Settings/Quit must still run their own action, picking it must close the Journal and open the menu. Log lines: `system menu: loading ...`, `system menu: '<name>' picked`. Check with SkyUI and with the vanilla Journal. Unverified: the `_global.InkwellSystemMenus` hand-over, `createEmptyMovieClip` with a numeric depth, and `$Key` labels.
