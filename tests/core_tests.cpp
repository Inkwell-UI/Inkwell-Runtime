// Unit tests for the engine-free core: version math, menu JSON validation, INI parser (shared vectors) and the small tables.
// No framework: a failed CHECK prints and the process exits non-zero. Usage: InkwellTests <path to test-vectors>
#include "core/Ini.h"
#include "core/MenuConfig.h"
#include "core/Registry.h"
#include "core/Version.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

using json = nlohmann::json;
using namespace inkwell;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                                      \
	do {                                                                                 \
		++g_checks;                                                                      \
		if (!(cond)) {                                                                   \
			++g_failed;                                                                  \
			std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n";   \
		}                                                                                \
	} while (0)
#define CHECK_MSG(cond, msg)                                                                        \
	do {                                                                                            \
		++g_checks;                                                                                 \
		if (!(cond)) {                                                                              \
			++g_failed;                                                                             \
			std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond " -- " << (msg) << "\n"; \
		}                                                                                           \
	} while (0)

static std::string ReadFile(const std::filesystem::path& p)
{
	std::ifstream     in(p, std::ios::binary);
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

// ---- versions ----------------------------------------------------------------------------------------------------
static void TestVersions()
{
	CHECK(Pack({ 0, 3, 0 }) == 196608);
	CHECK(Pack({ 1, 2, 3 }) == 16777216 + 2 * 65536 + 48);
	CHECK(kRuntimeVersion.major == 0 && Pack(kRuntimeVersion) >= 196608);
	CHECK(ParseVersion("0.3.0").has_value());
	CHECK(ParseVersion("0.0.4095").has_value());
	CHECK(!ParseVersion("0.0.4096"));
	CHECK(!ParseVersion("256.0.0"));
	CHECK(!ParseVersion("1.2"));
	CHECK(!ParseVersion("1.2.3.4"));
	CHECK(!ParseVersion("a.b.c"));
	CHECK(!ParseVersion(""));
	CHECK(!ParseVersion("1..2"));
	CHECK(ToString(*ParseVersion("12.34.567")) == "12.34.567");
	CHECK(IsCompatible({ 0, 3, 0 }, { 0, 3, 0 }));
	CHECK(IsCompatible({ 0, 3, 0 }, { 0, 4, 2 }));
	CHECK(!IsCompatible({ 0, 3, 1 }, { 0, 3, 0 }));
	CHECK(!IsCompatible({ 0, 3, 0 }, { 1, 0, 0 }));
}

// ---- swf path and menu JSON --------------------------------------------------------------------------------------
static void TestNormalizeSwf()
{
	CHECK(NormalizeSwfPath("Interface/Wheel.swf") == "Wheel");
	CHECK(NormalizeSwfPath("Interface/Inkwell/MyMod/Wheel.swf") == "Inkwell/MyMod/Wheel");
	CHECK(NormalizeSwfPath("interface/Wheel.SWF") == "Wheel");
	for (const char* bad : { "Wheel.swf", "Interface/Wheel", "Interface/../x.swf", "Interface/a/../x.swf", "C:/Interface/x.swf", "/Interface/x.swf", "Interface\\x.swf",
			 "Interface/.swf", "Interface//x.swf", "Data/Interface/x.swf", "Interface/x:y.swf", "", "Interface/./x.swf", "Interface/a/.swf" }) {
		CHECK_MSG(!NormalizeSwfPath(bad), bad);
	}
}

static json GoodMenu()
{
	return json::parse(R"({
	  "specVersion": 6, "name": "MyMod_Wheel", "swf": "Interface/Inkwell/MyMod/Wheel.swf", "minRuntime": "0.3.0",
	  "flags": { "pausesGame": true, "cursor": true, "modal": true, "closeOnEscape": true, "future": 1 }, "inputContext": "menuMode", "depth": 3,
	  "triggers": { "hotkey": 37, "papyrus": true, "modEvent": "MyMod_OpenWheel", "more": [1] }, "extra": { "a": 1 }, "unknownTop": { "x": 1 } })");
}

static MenuParseResult Parse(const json& j) { return ParseMenuDef(j.dump()); }
static std::vector<Problem> Errors(const MenuParseResult& r)
{
	std::vector<Problem> e;
	for (const auto& p : r.problems) {
		if (p.level == Problem::Level::kError) e.push_back(p);
	}
	return e;
}

static void TestMenuDef()
{
	{
		const auto r = Parse(GoodMenu());
		CHECK(r.def.has_value());
		if (r.def) {
			CHECK(r.def->name == "MyMod_Wheel" && r.def->bareSwf == "Inkwell/MyMod/Wheel");
			CHECK(r.def->pausesGame && r.def->cursor && r.def->modal && r.def->closeOnEscape);
			CHECK(r.def->inputContext == InputContext::kMenuMode && r.def->depth == 3);
			CHECK(r.def->hotkey == 37 && r.def->papyrus && r.def->modEvent == "MyMod_OpenWheel");
		}
		CHECK(!r.problems.empty());  // unknown keys are noted at debug level
		for (const auto& p : r.problems) CHECK(p.level == Problem::Level::kDebug);
	}
	{  // defaults
		const auto r = Parse({ { "specVersion", 6 }, { "name", "Ab" }, { "swf", "Interface/a.swf" } });
		CHECK(r.def.has_value());
		if (r.def) {
			CHECK(r.def->minRuntime == "0.3.0" && r.def->depth == 3);
			CHECK(!r.def->pausesGame && !r.def->cursor && !r.def->modal && r.def->closeOnEscape);
			CHECK(r.def->papyrus && !r.def->hotkey && r.def->modEvent.empty());
			CHECK(r.def->inputContext == InputContext::kNone);
		}
	}
	{  // modal implies menuMode when inputContext is absent; an explicit "none" wins
		auto j = json{ { "specVersion", 6 }, { "name", "Ab" }, { "swf", "Interface/a.swf" }, { "flags", { { "modal", true } } } };
		CHECK(Parse(j).def && Parse(j).def->inputContext == InputContext::kMenuMode);
		j["inputContext"] = "none";
		CHECK(Parse(j).def && Parse(j).def->inputContext == InputContext::kNone);
	}
	// every invalid field skips the file with an error naming the field
	struct Bad { const char* label; json patch; const char* field; };
	const std::vector<Bad> bad = {
		{ "name 1 char", { { "name", "A" } }, "name" }, { "name digit start", { { "name", "1abc" } }, "name" }, { "name dash", { { "name", "a-b" } }, "name" },
		{ "name 65", { { "name", std::string(65, 'a') } }, "name" }, { "name type", { { "name", 5 } }, "name" },
		{ "swf no Interface", { { "swf", "Foo/x.swf" } }, "swf" }, { "swf dotdot", { { "swf", "Interface/../x.swf" } }, "swf" }, { "swf colon", { { "swf", "Interface/x:.swf" } }, "swf" },
		{ "swf ext", { { "swf", "Interface/x.gfx" } }, "swf" }, { "minRuntime short", { { "minRuntime", "0.3" } }, "minRuntime" },
		{ "minRuntime 4 digits", { { "minRuntime", "0.3.1000" } }, "minRuntime" }, { "inputContext", { { "inputContext", "gameplay" } }, "inputContext" },
		{ "depth 16", { { "depth", 16 } }, "depth" }, { "depth -1", { { "depth", -1 } }, "depth" }, { "depth frac", { { "depth", 1.5 } }, "depth" },
		{ "hotkey 1", { { "triggers", { { "hotkey", 1 } } } }, "triggers.hotkey" }, { "hotkey 256", { { "triggers", { { "hotkey", 256 } } } }, "triggers.hotkey" },
		{ "modEvent space", { { "triggers", { { "modEvent", "a b" } } } }, "triggers.modEvent" },
		{ "modEvent 65", { { "triggers", { { "modEvent", std::string(65, 'a') } } } }, "triggers.modEvent" },
		{ "flag type", { { "flags", { { "modal", "yes" } } } }, "flags.modal" }, { "papyrus type", { { "triggers", { { "papyrus", 1 } } } }, "triggers.papyrus" },
		{ "specVersion string", { { "specVersion", "6" } }, "specVersion" }, { "specVersion old", { { "specVersion", 5 } }, "specVersion" },
		{ "flags not object", { { "flags", 3 } }, "flags" }, { "extra not object", { { "extra", 3 } }, "extra" },
	};
	for (const auto& b : bad) {
		auto j = GoodMenu();
		for (auto it = b.patch.begin(); it != b.patch.end(); ++it) j[it.key()] = it.value();
		const auto r = Parse(j);
		CHECK_MSG(!r.def.has_value(), b.label);
		CHECK_MSG(Errors(r).size() == 1 && Errors(r)[0].message.rfind(b.field, 0) == 0, b.label);
	}
	{  // boundaries that are valid
		auto j = GoodMenu();
		j["depth"] = 15;
		j["triggers"] = { { "hotkey", 255 } };
		CHECK(Parse(j).def.has_value());
		j["triggers"] = { { "hotkey", 2 } };
		CHECK(Parse(j).def.has_value());
		j["specVersion"] = 6.0;  // JS: an integral float is an int
		CHECK(Parse(j).def.has_value());
		j["specVersion"] = 7;  // newer than the runtime: parsed with a warning
		const auto r = Parse(j);
		CHECK(r.def.has_value() && std::any_of(r.problems.begin(), r.problems.end(), [](const Problem& p) { return p.level == Problem::Level::kWarn; }));
	}
	// not JSON, not an object, BOM, empty
	CHECK(!ParseMenuDef("{ nope").def && ParseMenuDef("{ nope").problems.size() == 1);
	CHECK(!ParseMenuDef("[1,2]").def);
	CHECK(!ParseMenuDef("").def);
	CHECK(!ParseMenuDef("\"x\"").def);
	CHECK(ParseMenuDef("\xEF\xBB\xBF" + GoodMenu().dump()).def.has_value());
	// the spike's form (singular `trigger`, no specVersion) is rejected with a clear line
	{
		const auto r = ParseMenuDef(R"({"name":"InkwellSpikeA","swf":"Interface/InkwellSpikeA.swf","trigger":{"hotkey":24}})");
		CHECK(!r.def && r.problems.size() == 1 && r.problems[0].message.rfind("specVersion", 0) == 0);
	}
	// S3 rule 4: minRuntime vs the running version
	{
		auto j = GoodMenu();
		j["minRuntime"] = "0.3.1";
		const auto r = ParseMenuDef(j.dump(), { 0, 3, 0 });
		CHECK(!r.def && Errors(r).size() == 1);
		CHECK(Errors(r)[0].message == "ERROR menu 'MyMod_Wheel' needs Inkwell Runtime 0.3.1, this is 0.3.0: not registered");
		j["minRuntime"] = "0.3.0";
		CHECK(ParseMenuDef(j.dump(), { 0, 4, 0 }).def.has_value());   // newer runtime, same major
		CHECK(!ParseMenuDef(j.dump(), { 1, 0, 0 }).def.has_value());  // other major
		j["minRuntime"] = "0.300.0";
		CHECK(!ParseMenuDef(j.dump(), { 0, 3, 0 }).def.has_value());  // passes the 3-digit shape, fails the 0-255 range
	}
}

// ---- INI vectors -------------------------------------------------------------------------------------------------
static json ToJson(const ParsedIni& p)
{
	json ini = json::object();
	ini["menus"] = json::object();
	for (const auto& [n, m] : p.menus) {
		json o = json::object();
		if (m.openKey) o["openKey"] = *m.openKey;
		if (m.uiScale) o["uiScale"] = *m.uiScale;
		if (m.offsetX) o["offsetX"] = *m.offsetX;
		if (m.offsetY) o["offsetY"] = *m.offsetY;
		if (m.theme) o["theme"] = *m.theme;
		ini["menus"][n] = o;
	}
	ini["widgets"] = json::object();
	for (const auto& [n, w] : p.widgets) {
		json o = json::object();
		if (w.offsetX) o["offsetX"] = *w.offsetX;
		if (w.offsetY) o["offsetY"] = *w.offsetY;
		if (w.scale) o["scale"] = *w.scale;
		ini["widgets"][n] = o;
	}
	if (p.hasRuntimeSection) ini["runtime"] = { { "logLevel", *p.logLevel } };
	if (p.hasDevSection) {
		json d = json::object();
		if (p.devMode) d["devMode"] = *p.devMode;
		if (p.port) d["port"] = *p.port;
		if (p.token) d["token"] = *p.token;
		if (p.watch) d["watch"] = *p.watch;
		ini["dev"] = d;
	}
	return ini;
}

static void TestIniVectors(const std::filesystem::path& dir)
{
	int count = 0;
	for (const auto& e : std::filesystem::directory_iterator(dir)) {
		const auto name = e.path().filename().string();
		if (e.path().extension() != ".ini") continue;
		++count;
		const auto stem = name.substr(0, name.size() - 4);
		const auto expected = json::parse(ReadFile(dir / (stem + ".expected.json")));
		const auto kind = expected.at("kind") == "runtime" ? IniKind::kRuntime : IniKind::kMod;
		const auto parsed = ParseIni(ReadFile(e.path()), kind);

		CHECK_MSG(ToJson(parsed) == expected.at("ini"), name + "\n  got:      " + ToJson(parsed).dump() + "\n  expected: " + expected.at("ini").dump());

		json problems = json::array();
		for (const auto& p : parsed.problems) problems.push_back({ { "line", p.line }, { "section", p.section }, { "key", p.key } });
		CHECK_MSG(problems == expected.at("problems"), name + "\n  got:      " + problems.dump() + "\n  expected: " + expected.at("problems").dump());

		for (const auto& r : expected.at("resolve")) {
			std::optional<int> hot;
			if (r.contains("jsonHotkey")) hot = r.at("jsonHotkey").get<int>();
			const auto eff = ResolveMenu(hot, parsed.FindMenu(r.at("menu").get<std::string>()));
			const auto& x = r.at("effective");
			const bool ok = (eff.openKey.has_value() == x.contains("openKey")) && (!eff.openKey || *eff.openKey == x.at("openKey").get<int>()) &&
			                eff.uiScale == x.at("uiScale").get<double>() && eff.offsetX == x.at("offsetX").get<int>() && eff.offsetY == x.at("offsetY").get<int>() &&
			                eff.theme == x.at("theme").get<std::string>();
			CHECK_MSG(ok, name + " resolve " + r.at("menu").get<std::string>());
		}
	}
	CHECK_MSG(count >= 7, "found only " + std::to_string(count) + " .ini vectors in " + dir.string());
}

static void TestIniMisc()
{
	// never throws on odd input
	for (const char* t : { "", "[", "]=", "=", "[Menu.]\nOpenKey=1", "\0\0", "[Menu.A]\nUIScale=1e9", "[Menu.A]\nOpenKey" }) {
		(void)ParseIni(t);
		(void)ParseIni(t, IniKind::kRuntime);
	}
	// getSetting copy: any section, lower-cased, last duplicate wins
	const auto p = ParseIni("[Custom Section]\nMyKey = a b \nmykey=last\n[Menu.A]\nOpenKey=5\n");
	CHECK(p.raw.at("custom section").at("mykey") == "last");
	CHECK(p.raw.at("menu.a").at("openkey") == "5");
	// a mod INI cannot switch dev mode on
	CHECK(!ParseIni("[Dev]\nDevMode=1\n").devMode.has_value());
	CHECK(ParseIni("[Dev]\nDevMode=1\n", IniKind::kRuntime).devMode == true);
}

// ---- tables ------------------------------------------------------------------------------------------------------
static void TestTables()
{
	DataStore ds;
	ds.Set("Menu_A", "k1", std::string("one"));
	ds.Set("menu_a", "k2", 2.0);
	ds.Set("MENU_A", "k1", true);  // k1 again: last value, moves to the end
	const auto snap = ds.Snapshot("Menu_a");
	CHECK(snap.size() == 2 && snap[0].key == "k2" && snap[1].key == "k1");
	CHECK(snap.size() == 2 && std::get<bool>(snap[1].value) == true && std::get<double>(snap[0].value) == 2.0);
	CHECK(ds.Snapshot("other").empty());

	FunctionTable ft;
	CHECK(ft.Register("inkwell.version", [](const std::string&) { return std::string("x"); }).empty());
	CHECK(!ft.Register("inkwell.version", [](const std::string&) { return std::string("y"); }).empty());  // duplicate
	CHECK(!ft.Register("Bad Name", [](const std::string&) { return std::string(); }).empty());
	CHECK(!ft.Register("ab", [](const std::string&) { return std::string(); }).empty());
	CHECK(!ft.Register("UPPER.case", [](const std::string&) { return std::string(); }).empty());
	CHECK(ft.Register("my_plugin.echo", [](const std::string& a) { return a + "!"; }).empty());
	CHECK(ft.Call("my_plugin.echo", "hi") == "hi!");
	CHECK(ft.Call("MY_PLUGIN.ECHO", "hi") == "hi!");
	CHECK(!ft.Call("nope.nope", "").has_value());
	CHECK(ft.Size() == 2);
}

int main(int argc, char** argv)
{
	const std::filesystem::path vectors = argc > 1 ? argv[1] : "test-vectors";
	TestVersions();
	TestNormalizeSwf();
	TestMenuDef();
	TestIniVectors(vectors / "ini");
	TestIniMisc();
	TestTables();
	std::cout << g_checks << " checks, " << g_failed << " failed\n";
	return g_failed == 0 ? 0 : 1;
}
