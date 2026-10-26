# Inkwell Runtime

SKSE plugin that registers Inkwell custom menus, bridges Papyrus, AS2 and C++, keeps saved state, and hosts the editor's dev tools (hot reload, live inspection). The full design is in `Inkwell Design Spec.md` (workspace root), section "Inkwell Runtime"; read it before changing behavior. Where this file and the spec disagree, the spec wins; fix this file.

## Commands

The plugin is at the repo root (`CMakeLists.txt`, `src/`, `include/`, `papyrus/`); `spikes/` is history and stays untouched. Local build: set `VCPKG_ROOT` (`C:/dev/vcpkg` here), then `cmake --preset release`, `cmake --build --preset release` and `ctest --preset release` (the first CommonLibSSE-NG build takes about 15 minutes). For the engine-free core and its unit tests only, use the `tests` preset (seconds). CI: `.github/workflows/runtime.yml` on a Windows runner (build, ctest, artifact `Inkwell-dll`; it prints the configure/build log tail as annotations). The preset names no Visual Studio generator, because runner images change. Every change must build in CI. `src/core` has no engine headers, so its logic is unit-tested; `test-vectors/ini/` is shared with `Inkwell-Spec` (the INI parser must give the same result as the Spec's `parseIni`). `docs/test-notes.md` holds the owner's in-game steps (section 12 is dev mode). `scripts/dev-client.mjs` checks the dev server without the editor. Papyrus: `papyrus.exe compile -nocache -i papyrus/Source/Scripts -o <out>` (russo compiler, no stubs needed); `.pex` files are never committed.

## Stack

C++, CommonLibSSE-NG (alandtse's fork, GPL-3.0-or-later), Address Library, spdlog. One DLL for Skyrim SE, AE and GOG. **`Inkwell.dll` is licensed GPL-3.0** because it links CommonLibSSE-NG; the no-paywall terms live in the editor and generated-output licenses, not here.

## Rules that are easy to break

- **Names are API once public:** `Inkwell.dll`, `SKSE/Plugins/Inkwell/`, `Inkwell_API.psc`, `inkwell.*` in AS2. Keep the product name in one constant; never rename after the first public release.
- **Backwards compatible within a major version.** Mods declare the minimum runtime version they need; a missing or too-old runtime shows an in-game message naming the requirement, never a silent failure.
- **Config formats come from inkwell-spec:** author-owned menu JSON (`SKSE/Plugins/Inkwell/menus/*.json`) is read first, then end-user INI overrides on top. Validate against the spec's schemas; never invent fields here.
- **`LoadMovieEx` takes a bare movie name** and adds `Interface/` and `.swf` itself. The menu JSON `swf` is relative to `Data/` including `Interface/` and `.swf`; normalize it before calling. `UI::Register` takes a context-free function pointer, so each menu needs its own template slot.
- **`skse` is not present in a custom menu's movie.** Bridge functions (such as `_root.inkwellTrace`) are installed by the runtime after the movie loads, so frame-1 AS2 must retry until they exist.
- **Never crash the game on bad input:** a malformed menu file, missing SWF or bad Papyrus call logs and skips that item.
- **Dev mode (S6) is gated by `Inkwell.ini [Dev] DevMode=1` only** (never a mod INI; one DLL for everyone). Off = no socket, no watcher, no token. On: loud warn banner, WebSocket on `127.0.0.1` only, token (64 hex, constant-time compare, masked in the log, generated and written back to `Inkwell.ini` if empty), refuse any request with an `Origin` header or a foreign `Host`, one editor at a time, 1 MiB messages, hello within 5 s. The server and codec live in `src/core` (Winsock, no engine; `tests/dev_tests.cpp`); `src/plugin/Dev.cpp` is the glue. **Network threads never touch the engine:** they queue to the game thread with `SKSE::GetTaskInterface()`. Hot reload mutates a `MenuEntry` only while its menu is closed, on the game thread; slots are never freed (removal reserves, a new name needs a free slot).
- **Hotkeys, positions, scale, theme and UI scale are user-owned (INI).** Never hard-code them.
- **Saved state goes through SKSE co-save,** versioned so older saves still load: unique id `INKW`, one `SVAR` record per menu, layout and limits in S5 (`src/core/Saved.*`, constants mirror Spec `COSAVE_*`/`SAVED_LIMITS`). A record version the runtime does not know is skipped and logged, never fatal. A change to the byte layout needs a new record version (readers keep decoding the old one).
- **Log generously with spdlog.** In-game testing is done by hand, so each run must tell as much as possible.
- **Testing:** Claude can't run Skyrim. Keep a small test menu and step-by-step test notes in the repo for the author to run.
