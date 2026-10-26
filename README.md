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
CMakeLists.txt, CMakePresets.json, vcpkg.json   build (product name in one place: INKWELL_PRODUCT_NAME in CMakeLists.txt)
src/core/        pure C++ (no engine): versions, menu JSON validation, INI parser, saved-variable store and co-save record codec, small tables, and the dev server (Crypto, WebSocket, DevProtocol, DevServer on Winsock, DirWatcher, MenuScan). Unit-tested.
src/plugin/      the SKSE plugin: Runtime (registry), Menu (IMenu), Bridge (AS2), Input, Papyrus, Messaging, main
include/InkwellAPI.h   C interface for other SKSE plugins (MIT)
papyrus/Source/Scripts/Inkwell_API.psc   the Papyrus API (ship it with the runtime; compile it with your Papyrus compiler)
papyrus/stubs/   tiny original header stubs, used only to compile the test helper script
tests/           core_tests.cpp (CTest `core`), dev_tests.cpp (CTest `dev`: dev server unit tests and a loopback end-to-end run; `InkwellDevTests --serve <port> <token>` keeps a fake server up)
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

Needs Visual Studio 2022 (or newer) with the C++ workload, CMake 3.21+, git and a vcpkg checkout (CI pins commit `23b8a8e7409c2d6346114a88e834165bde79a51c`).

```powershell
git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
git -C C:\dev\vcpkg checkout 23b8a8e7409c2d6346114a88e834165bde79a51c
$env:VCPKG_ROOT = "C:\dev\vcpkg"      # bash: export VCPKG_ROOT=C:/dev/vcpkg

# the whole thing: Inkwell.dll + unit tests (the first CommonLibSSE-NG build takes about 15 minutes)
cmake --preset release
cmake --build --preset release
ctest --preset release            # builds nothing; runs the two test programs (core, dev). The DLL is build\Release\Inkwell.dll

# only the engine-free core and its tests (seconds, no CommonLibSSE-NG)
cmake --preset tests
cmake --build --preset tests
ctest --preset tests
```

The presets are `release` (plugin + tests; configure, build and test presets share the name) and `tests` (engine-free core only). The release build needs a long time the first time because vcpkg builds CommonLibSSE-NG; later builds are incremental. The DLL embeds a build timestamp, so two builds of the same source have different SHA-256 values.

## CI

`.github/workflows/runtime.yml` (Windows runner) configures with the `release` preset, builds, runs `ctest` and uploads the `Inkwell-dll` artifact on every push. It has never run on GitHub for 0.3 (nothing is pushed). The DLL, PDB and `.pex` files are never committed.

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
- Dependencies keep their own licences (CommonLibSSE-NG, spdlog, fmt and the other vcpkg ports: see `vcpkg.json`). The Papyrus compiler, SKSE and the game files are never bundled.
