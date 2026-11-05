set_xmakever("3.0.0")

-- The product name and version live here and nowhere else; C++ gets the name as INKWELL_PRODUCT_NAME.
local PRODUCT_NAME = "Inkwell"
local VERSION = "0.6.0"

set_project(PRODUCT_NAME)
set_version(VERSION)
set_languages("c++23")
set_encodings("utf-8")
add_rules("mode.debug", "mode.release")
set_defaultmode("release")
-- static libs, dynamic CRT (the old x64-windows-static-md triplet)
set_runtimes(is_mode("debug") and "MDd" or "MD")

-- Option names matter: `plugin` breaks package resolution once CommonLibSSE-NG is included, `tests` is CommonLibSSE-NG's own option.
option("build_plugin", {default = true, showmenu = true, description = "Build Inkwell.dll (needs CommonLibSSE-NG; the first build takes several minutes)"})
option("core_tests", {default = true, showmenu = true, description = "Build and register the core unit tests"})
-- Until 1.0 the log defaults to trace so every in-game test run tells as much as possible (S2 says "info" for the final default).
option("default_log_level", {default = "trace", showmenu = true, description = "Log level used when Inkwell.ini does not set [Runtime] LogLevel"})

add_requires("nlohmann_json")

-- Pinned CommonLibSSE-NG release (alandtse fork, v13.1.0). Supports 1.7.104; see README.
-- Fetched by scripts/fetch-commonlib.ps1 (xmake.lua cannot run git); run that once before the first `xmake f`.
-- shortcut: a script, not a git submodule; switch to a submodule if you want git to track the pin.
if has_config("build_plugin") then
    includes("extern/CommonLibSSE-NG")
end

-- ---- core: pure C++ (menu JSON, INI, versions, tables). No engine headers, so the tests build in seconds. ----
target("InkwellCore")
    set_kind("static")
    add_files("src/core/*.cpp")
    add_includedirs("src", {public = true})
    local major, minor, patch = VERSION:match("(%d+)%.(%d+)%.(%d+)")
    add_defines("INKWELL_PRODUCT_NAME=\"" .. PRODUCT_NAME .. "\"",
                "INKWELL_VERSION_MAJOR=" .. major,
                "INKWELL_VERSION_MINOR=" .. minor,
                "INKWELL_VERSION_PATCH=" .. patch, {public = true})
    add_packages("nlohmann_json", {public = true})
    add_syslinks("ws2_32", "bcrypt", {public = true})
    if is_plat("windows") then
        add_cxflags("/W4", {public = true, force = true})
    end

-- ---- the SKSE plugin: Inkwell.dll (Address Library, struct-independent, no hard-coded runtime: SE, AE and GOG) ----
if has_config("build_plugin") then
    target(PRODUCT_NAME)
        set_version(VERSION)
        add_rules("commonlibsse-ng.plugin", {name = PRODUCT_NAME, author = "Inkwell", description = "Inkwell"})
        add_files("src/plugin/*.cpp")
        add_includedirs("src", "include")
        add_defines("INKWELL_PRODUCT_NAME=\"" .. PRODUCT_NAME .. "\"",
                    "INKWELL_DEFAULT_LOG_LEVEL=\"" .. (get_config("default_log_level") or "trace") .. "\"")
        add_deps("InkwellCore")
        set_pcxxheader("src/plugin/PCH.h")
        -- CommonLib's rule installs into %XSE_TES5_MODS_PATH% after every build; only do that when asked (INKWELL_INSTALL=1).
        on_config(function(target)
            if not os.getenv("INKWELL_INSTALL") then
                target:set("installdir", path.join(os.projectdir(), "build", "install"))
            end
        end)
        -- a PDB next to the DLL (Release stays optimized): crash logs and debuggers can resolve the addresses
        set_symbols("debug")
        add_ldflags("/DEBUG", "/OPT:REF", "/OPT:ICF", {force = true})
end

if has_config("core_tests") then
    target("InkwellTests")
        set_kind("binary")
        add_files("tests/core_tests.cpp")
        add_deps("InkwellCore")
        add_tests("core", {runargs = {path.join(os.projectdir(), "test-vectors")}})

    -- dev server (S6): unit tests plus a loopback end-to-end run, no game needed
    target("InkwellDevTests")
        set_kind("binary")
        add_files("tests/dev_tests.cpp")
        add_deps("InkwellCore")
        add_tests("dev", {runargs = {path.join(os.projectdir(), "test-assets")}})
end
