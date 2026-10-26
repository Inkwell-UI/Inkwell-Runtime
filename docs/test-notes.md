# In-game test notes: Inkwell Runtime 0.3, phase 2 (runtime core)

For the owner. Claude cannot run Skyrim, so nothing below has been seen in game yet. Each step says what you should see; the lines in `Inkwell.log` are the evidence. Please send back `Inkwell.log`, `skse64.log` and the answers to the "Report" questions at the end.

Tested game so far for the spikes: Skyrim SE 1.7.104 (Steam). AE and GOG are unverified (decision 3).

## 0. What you need

| What | Where it comes from |
|---|---|
| `Inkwell.dll` | the CI artifact `Inkwell-dll` of the workflow `runtime` (GitHub, Actions, latest run on branch `feat/runtime-core`), or `build\Release\Inkwell.dll` after a local build |
| Address Library for SKSE Plugins (v13 or newer for 1.7.104) | already installed for the spikes |
| `InkwellSpikeA.swf` ... `InkwellSpikeD.swf` | the MO2 mod `Inkwell Spikes` from the 0.0 spikes (`Interface/`). Keep it enabled. They are the only SWFs that exist until the editor's menu target (phase 3) is done. They do not call `glueReady()`, so each open waits 5 s for it and logs a warning: that is expected here |
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
   - `Inkwell Runtime 0.3.0 (spec 6, protocol 1) on Skyrim 1-7-104-0, SKSE ...`
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

`cgf` does not exist in this Skyrim SE, and a Papyrus function can only be called from a script that is attached to something (a quest, an alias, a magic effect). `InkwellTestHelper` and `Inkwell_API` are `Hidden` global-function scripts: nothing in this repo attaches them, and the helper cannot register for keys (`RegisterForKey` needs a `Quest`/`Form` script object). The Editor's generated ESP (`src/main/widget/esp.ts`) attaches a script to one start-game-enabled quest, but it is hand-written for HUD widgets (master `SkyUI_SE.esp`, one fixed script) and a menu mod deploys no ESP at all; the generated no-master ESP is Phase 7. So for this phase:

| Check | Without an attached script |
|---|---|
| Open and close a menu with a hotkey, Esc, flags, INI overrides, bad files, slot limit (sections 1, 2, 5, 6, 7) | **Doable**, no Papyrus |
| The AS2 bridge (section 4) and the editor slice (`TEST-IN-GAME-0.3-slice.md`) | **Doable**, hotkey from the menu JSON |
| `Inkwell_API.Open/Close/IsMenuOpen/IsMenuRegistered/SetString/SetNumber/SetBool/GetVersion` | **Not doable.** Needs a script. The C++ side is covered only by the `registered functions` log line at kDataLoaded |
| `triggers.papyrus=false` refusal (menu D), unknown menu name | **Not doable** (Papyrus call) |
| Mod event open (`triggers.modEvent`) and `Inkwell_MenuOpened/Closed` receivers | **Not doable** (needs `SendModEvent`, a script). Another installed mod that sends a mod event could trigger it, but none is known to |
| Menu C (its hotkey is removed by the INI) | Doable by editing the INI: delete the `[Menu.InkwellTest_C]` block of `InkwellTest.ini` and C opens with **L** |

What you can still read without a script: the `Inkwell_MenuOpened`/`Inkwell_MenuClosed` lines in `Inkwell.log` (`send mod event ... strArg='InkwellTest_A'`) on every open and close of a hotkey menu, which shows the runtime sends them.

**If you want the Papyrus tests anyway** (optional, needs the Creation Kit): make a plugin with one start-game-enabled quest, attach a script `extends Quest` that calls `RegisterForKey(<scan code>)` in `OnInit`/`OnPlayerLoadGame` and, in `OnKeyDown`, calls `Inkwell_API.Open("InkwellTest_C")`, `Inkwell_API.Open("InkwellTest_D")` (must be refused), `Inkwell_API.Open("NoSuchMenu")`, `Inkwell_API.Close(...)`, `Inkwell_API.SetString("InkwellTest_A", "greeting", "hello")`, and `Debug.Notification(Inkwell_API.GetVersionString())`. Compile with `D:\Mod Tools\Russo Compiler\papyrus.exe compile -nocache -i <src> -o <out>`. Expected log lines are the ones the old console steps listed: `open 'InkwellTest_C' (papyrus): queue kShow`, `refused, the menu JSON has triggers.papyrus=false`, `open 'NoSuchMenu' (papyrus): unknown menu`, `set data 'InkwellTest_A'.greeting = "hello" (papyrus)`, and `mod event 'InkwellTest_Open' -> open 'InkwellTest_A'` for `SendModEvent`. `InkwellTestHelper.SendOpenEvent` / `ReportVersion` can be called from that script. This is not done by Claude and not verified.

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
| `bad_06_needs_newer_runtime.json` | `skipped, ERROR menu 'InkwellBad_NewerRuntime' needs Inkwell Runtime 0.4.0, this is 0.3.0: not registered` |
| `bad_07_other_major.json` | the same text with `1.0.0` |
| `zz_08_duplicate_name.json` | `skipped, menu name 'inkwelltest_a' is already used by ...InkwellTest_A.json` |
| `bad_09_engine_name.json` | loads (valid JSON) but then `skipped, the engine already has a menu with this name (vanilla or another plugin)` at `UI::Register` time. It takes a template slot |
| `bad_10_depth.json` | `skipped, depth: expected a whole number from 0 to 15` |

Slot limit: also copy `test-assets/menus-slots/` (14 tiny menus, files sort after the test menus). Expected: `slot exhaustion: InkwellFill_NN not registered (...: all 16 template slots are in use)` for the surplus ones, and, after you load a save, one notification `Inkwell: N menus were not registered (limit 16 menus). See Inkwell.log.`

## 6. INI overrides

Edit `InkwellTest.ini` (the file in your MO2 mod), change `OpenKey` of `[Menu.InkwellTest_B]`, restart, and check `ini: ...; effective hotkey=` in the log and the new key in game. Delete the whole file: the JSON hotkeys apply again (P opens B). An invalid value only produces a `warn` line with the file, line number and key.

## 7. Runtime-wide Inkwell.ini

Copy `test-assets/ini/Inkwell.ini` to `SKSE/Plugins/Inkwell/Inkwell.ini`. Expected: `log level 'debug' (from Inkwell.ini)` (trace lines disappear). Setting `DevMode=1` only logs `[Dev] DevMode=1 is set, but the dev server ... is not part of this build`. A `[Dev]` or `[Runtime]` section in `InkwellTest.ini` is ignored with a warning (a mod cannot switch dev mode on).

## 8. What I could not check (please look)

- Everything above, in game. The flag mapping (`pausesGame`, `cursor`, `modal`, `inputContext` to engine flags) is the spike's guess plus one change: `kUsesMenuContext` and `inputContext = menuMode` now follow the JSON `inputContext` (default: menuMode when `modal`), not `modal` itself.
- The hotkey is ignored while `IsApplicationMenuOpen()`, `IsModalMenuOpen()` or `GameIsPaused()` is true. If the hotkey never opens a menu in plain gameplay, the log line `ignored ...: <reason>` tells which test fires.
- `RegisterListener(nullptr, ...)` (listen to messages from every plugin, used for `kGetInterface`). If SKSE rejects a null sender the log says `listener ... registered: false`; the broadcast `kInterfaceReady` still works.
- Sub-path SWF names (`Interface/Inkwell/<modId>/<Swf>.swf` giving `LoadMovieEx 'Inkwell/<modId>/<Swf>'`, O2): the missing-SWF menu only proves failure handling; a real sub-path SWF is tested in the phase 4 slice.
- AE and GOG.

## Report

Please send `Inkwell.log`, `skse64.log`, and: (1) did the DLL load; (2) per menu A to D: paused, cursor, movement, Esc behaviour; (3) which hotkeys were ignored and why; (4) did the missing-SWF menu crash, hang or only notify; (5) any `error` or `warn` line you did not expect.
