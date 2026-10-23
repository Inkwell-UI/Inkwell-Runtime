# Inkwell Runtime

SKSE plugin that registers Inkwell custom menus, bridges Papyrus, AS2 and C++, keeps saved state, and hosts the editor's dev tools (hot reload, live inspection). The full design is in `Inkwell Design Spec.md` (workspace root), section "Inkwell Runtime"; read it before changing behavior. Where this file and the spec disagree, the spec wins; fix this file.

## Commands

Not scaffolded yet. Builds run on a GitHub Actions Windows runner; list the local and CI build commands here once they exist. Every change must build in CI.

## Stack

C++, CommonLibSSE-NG (alandtse's fork), Address Library, spdlog. One DLL for Skyrim SE, AE and GOG.

## Rules that are easy to break

- **Names are API once public:** `Inkwell.dll`, `SKSE/Plugins/Inkwell/`, `Inkwell_API.psc`, `inkwell.*` in AS2. Keep the product name in one constant; never rename after the first public release.
- **Backwards compatible within a major version.** Mods declare the minimum runtime version they need; a missing or too-old runtime shows an in-game message naming the requirement, never a silent failure.
- **Config formats come from inkwell-spec:** author-owned menu JSON (`SKSE/Plugins/Inkwell/menus/*.json`) is read first, then end-user INI overrides on top. Validate against the spec's schemas; never invent fields here.
- **Never crash the game on bad input:** a malformed menu file, missing SWF or bad Papyrus call logs and skips that item.
- **Dev mode is off in release builds** unless the user INI enables it. The debug WebSocket binds to localhost only and requires the session token.
- **Hotkeys, positions, scale, theme and UI scale are user-owned (INI).** Never hard-code them.
- **Saved state goes through SKSE co-save,** versioned so older saves still load.
- **Log generously with spdlog.** In-game testing is done by hand, so each run must tell as much as possible.
- **Testing:** Claude can't run Skyrim. Keep a small test menu and step-by-step test notes in the repo for the author to run.
