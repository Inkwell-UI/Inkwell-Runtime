# Inkwell Runtime

The SKSE plugin (`Inkwell.dll`) behind Inkwell custom menus: it registers menus described by JSON files, opens them from a hotkey, a Papyrus call or a mod event, bridges Papyrus, AS2 and C++, and logs everything to `Documents/My Games/Skyrim Special Edition/SKSE/Inkwell.log`.

Status: milestone 0.3 (version 0.3.0, spec 6, runtime protocol 1): runtime core, saved state, runtime events, dev mode. Built against `Inkwell Design Spec.md` (Inkwell Runtime) and `SPEC-CHANGES.md` section S. **Nothing has been seen in game yet.** The owner's test is `TEST-IN-GAME-0.3.md` in the workspace root; `docs/test-notes.md` is the detailed reference (log lines per step) and lists what is unverified. Only Skyrim SE 1.7.104 is a target of the 0.3 tests; AE and GOG are unverified.

## What is in 0.3

| Area | State |
|---|---|
| Menu registry: `Data/SKSE/Plugins/Inkwell/menus/*.json`, validated like the Spec `MenuDef`, per-file skip with a log line, 16 template slots, `minRuntime` check | done |
| User INI overrides (`<modId>.ini`, `Inkwell.ini`), precedence default < JSON < INI | done |
| `InkwellMenu`: flags, depth, input context, `LoadMovieEx` with the normalized bare name, open/close | done |
| Triggers: hotkey (JSON or INI `OpenKey`), Esc, Papyrus, mod event | done |
| Papyrus API `Inkwell_API.psc` | done (`IsDevMode` is true when `Inkwell.ini [Dev] DevMode=1`) |
| AS2 bridge `_root.inkwell` | done (`watch`/`pin` stream to the editor in dev mode, no-ops otherwise) |
| SKSE messaging `InkwellAPI.h` (interface, `RegisterFunction`, menu messages) | done |
| Saved variables: in-memory store, SKSE co-save `INKW`/`SVAR` v1 (S5), `getSaved`/`setSaved`; unknown record versions are skipped and logged | done (not seen in game) |
| Dev mode (S6): `[Dev] DevMode=1` gate, WebSocket server on 127.0.0.1 with token, Origin/Host checks, trace forwarding, subscribe/watch/pin, `menuState`, hot reload (file watcher and the editor's `reload`) | done, engine-free parts unit-tested and run end to end on loopback; the game side is not seen in game |
| Missing-runtime message | the Editor generates it (`<modId>_RuntimeCheck.esp` + Papyrus, `Inkwell-Editor/docs/menus.md`); the runtime answers `SKSE.GetPluginVersion("Inkwell")` (packed `SKSEPlugin_Version`) and skips menus whose `minRuntime` it cannot meet |
| UI scale | `UIScale`, `OffsetX`, `OffsetY`, `Theme` are read from the INI and handed to the menu as `inkwell.uiScale` etc.; the Editor's generated menu class applies them to `_root` (unverified in GFx) |

## Layout

```
xmake.lua                                       build (product name and version in one place: PRODUCT_NAME and VERSION in xmake.lua)
src/core/        pure C++ (no engine): versions, menu JSON validation, INI parser, saved-variable store and co-save record codec, small tables, and the dev server (Crypto, WebSocket, DevProtocol, DevServer on Winsock, DirWatcher, MenuScan). Unit-tested.
src/plugin/      the SKSE plugin: Runtime (registry), Menu (IMenu), Bridge (AS2), Input, Papyrus, Messaging, main
include/InkwellAPI.h   C interface for other SKSE plugins (MIT)
papyrus/Source/Scripts/Inkwell_API.psc   the Papyrus API (ship it with the runtime; compile it with your Papyrus compiler)
papyrus/stubs/   tiny original header stubs, used only to compile the test helper script
tests/           core_tests.cpp (xmake test `core`), dev_tests.cpp (xmake test `dev`: dev server unit tests and a loopback end-to-end run; `InkwellDevTests --serve <port> <token>` keeps a fake server up)
scripts/         dev-client.mjs: a dependency-free dev-mode client for checking the server without the editor
test-vectors/    INI test vectors shared with Inkwell-Spec (see below)
test-assets/     test menus, bad menu files, example INIs, a test helper script
docs/test-notes.md   step-by-step in-game test for the author
spikes/          history: the 0.0 spikes (untouched)
```

## Installed layout

```
Data/SKSE/Plugins/Inkwell.dll                 the runtime (one DLL; SKSE loads it)
Data/SKSE/Plugins/Inkwell/menus/*.json        author menu files (written by the Editor, one per menu)
Data/SKSE/Plugins/Inkwell/<modId>.ini         end-user settings of one mod (created once by the Editor, never overwritten)
Data/SKSE/Plugins/Inkwell/Inkwell.ini         runtime-wide settings (LogLevel, [Dev]); never shipped in a released mod
Data/Interface/Inkwell/<modId>/<Swf>.swf      menu movies (the flat fallback is Interface/<modId>_<Swf>.swf)
Data/Scripts/Inkwell_API.pex                  compile papyrus/Source/Scripts/Inkwell_API.psc yourself; not built by CI
Documents/My Games/Skyrim Special Edition/SKSE/Inkwell.log   the runtime log
```

Requires SKSE64 and Address Library for SKSE Plugins (v13 or newer for 1.7.104).

## Build

Needs Visual Studio 2022 (or newer) with the C++ workload, [xmake](https://xmake.io) 3.0+ and git. Dependencies come from xmake-repo. CommonLibSSE-NG v13.1.0 (alandtse fork) is fetched into `extern/CommonLibSSE-NG` by a script, because `xmake.lua` cannot run git.

```powershell
.\scripts\fetch-commonlib.ps1     # once; does nothing when the pinned tag is already there

# the whole thing: Inkwell.dll + unit tests (the first CommonLibSSE-NG build takes several minutes)
xmake f -y -p windows -a x64 -m release
xmake -y
xmake test                        # runs the two test programs (core, dev). The DLL is build\windows\x64\release\Inkwell.dll

# only the engine-free core and its tests (seconds, no CommonLibSSE-NG)
xmake f -y -p windows -a x64 -m release --build_plugin=n
xmake -y
xmake test
```

Always pass `-p windows -a x64`: from Git Bash xmake otherwise picks MinGW. The options are `--build_plugin` and `--core_tests` (both default on) and `--default_log_level` (default `trace`: until 1.0 the log defaults to trace so every in-game test run tells as much as possible). Do not rename the first two to `plugin` or `tests`: `plugin` breaks package resolution once CommonLibSSE-NG is included, and `tests` is CommonLibSSE-NG's own option. A build installs nothing; set `INKWELL_INSTALL=1` to let CommonLibSSE-NG's rule install the DLL into `%XSE_TES5_MODS_PATH%` (otherwise it goes to `build\install`). `..\push-runtime.ps1` builds and copies the DLL, PDB and SWF into the MO2 mod. The DLL embeds a build timestamp, so two builds of the same source have different SHA-256 values.

Not verified: that the xmake-built DLL loads in game (it exports the same `SKSEPlugin_Load`, `SKSEPlugin_Query` and `SKSEPlugin_Version` as the earlier CMake build), and whether xmake-repo's package versions match the vcpkg ones the plugin was last tested with (CommonLibSSE-NG pins its own: DirectXMath 2024.02, DirectXTK 24.2.0, spdlog 1.16.0, rapidcsv 8.92).

## CI

`.github/workflows/runtime.yml` (Windows runner) installs xmake, runs `scripts/fetch-commonlib.ps1`, configures, builds, runs `xmake test` and uploads the `Inkwell-<version>` package artifact on every push. The workflow was ported from CMake and has not run on GitHub yet (nothing is pushed). The DLL, PDB and `.pex` files are never committed.

The Papyrus script compiles with the russo compiler (`INKWELL_PAPYRUS` / `D:\Mod Tools\Russo Compiler\papyrus.exe`); no header stubs are needed:

```powershell
papyrus.exe compile -nocache -i papyrus\Source\Scripts -o <out dir>
```

The log level defaults to `trace` until 1.0 (so every in-game test run tells as much as possible); `Inkwell.ini [Runtime] LogLevel` overrides it, and `-DINKWELL_DEFAULT_LOG_LEVEL=info` changes the built-in default.

## Dev mode

Off by default: no socket, no file watcher, no token, `watch`/`pin` are no-ops. It is switched on **only** by `Data/SKSE/Plugins/Inkwell/Inkwell.ini` (never by a mod INI):

```ini
[Dev]
DevMode=1
; Port=8788  Watch=1  Token=<64 hex digits; empty = the runtime makes one and writes it here>
```

Then the log shows a loud `INKWELL DEV MODE IS ON` banner and `dev server on 127.0.0.1:8788, token ab12...ef90` (masked). The server accepts one client with the token (sent in `hello`), refuses any request with an `Origin` header or a foreign `Host`, streams `trace`, `watch`, `pin` and `menuState`, and hot-reloads menus (file watcher, 300 ms debounce, or the editor's `reload` message). The Inkwell Editor connects by itself (its Dev deploy writes `Inkwell.ini` and a token: `Inkwell-Editor/docs/dev-mode.md`). Without the editor: `node scripts/dev-client.mjs --ini <Inkwell.ini> --seconds 30` (Node 22+, no packages). Details and the expected log lines: `docs/test-notes.md` section 12. The protocol is `RUNTIME_PROTOCOL` 1 of `Inkwell-Spec`; whether the engine re-reads a changed SWF on hot reload is unverified.

## Config formats

The formats are owned by `Inkwell-Spec` (`src/menu.ts`, `src/ini.ts`, `src/runtime.ts`). `src/core` mirrors them in C++; `test-vectors/ini/` holds INI inputs with the expected parse (generated by running the Spec's `parseIni`/`resolveMenu`), and `tests/core_tests.cpp` runs them, so the two parsers cannot drift apart silently. The Spec repo should adopt these vectors (its S2 text already says the C++ parser must pass the shared vectors in `test-vectors/ini/`).

## Licences

- `Inkwell.dll` and all source in this repository (except `include/`): **GPL-3.0-or-later** (`LICENSE`), because the DLL links CommonLibSSE-NG (GPL-3.0-or-later with a Modding Exception and a Linking Exception covering the game, SKSE and Windows). The no-paywall terms live in the editor and generated-output licences, not here.
- `include/InkwellAPI.h`: **MIT** (`include/LICENSE`), so any SKSE plugin can use the messaging interface without taking on the GPL. Not legal advice.
- Dependencies keep their own licences (CommonLibSSE-NG, spdlog, nlohmann-json, DirectXMath and the other xmake packages: see `xmake.lua` and CommonLibSSE-NG's `xmake.lua`). The Papyrus compiler, SKSE and the game files are never bundled.
