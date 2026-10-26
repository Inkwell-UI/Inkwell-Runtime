#include "Papyrus.h"

namespace inkwell
{
	namespace
	{
		constexpr auto kScript = "Inkwell_API"sv;

		std::int32_t      GetVersion(RE::StaticFunctionTag*) { return static_cast<std::int32_t>(Pack(kRuntimeVersion)); }
		RE::BSFixedString GetVersionString(RE::StaticFunctionTag*) { return RE::BSFixedString(ToString(kRuntimeVersion).c_str()); }

		// Dev mode (S6) is not part of this build: always false, even if Inkwell.ini asks for it (logged at load).
		bool IsDevMode(RE::StaticFunctionTag*) { return false; }

		bool IsMenuRegistered(RE::StaticFunctionTag*, RE::BSFixedString a_menu)
		{
			const bool r = Runtime::Get().Find(a_menu.c_str()) != nullptr;
			log::debug("Papyrus IsMenuRegistered('{}') -> {}", a_menu.c_str(), r);
			return r;
		}

		bool IsMenuOpen(RE::StaticFunctionTag*, RE::BSFixedString a_menu)
		{
			const bool r = Runtime::Get().IsMenuOpen(a_menu.c_str());
			log::debug("Papyrus IsMenuOpen('{}') -> {}", a_menu.c_str(), r);
			return r;
		}

		bool Open(RE::StaticFunctionTag*, RE::BSFixedString a_menu) { return Runtime::Get().OpenMenu(a_menu.c_str(), "papyrus", true); }
		bool Close(RE::StaticFunctionTag*, RE::BSFixedString a_menu) { return Runtime::Get().CloseMenu(a_menu.c_str(), "papyrus"); }

		void SetString(RE::StaticFunctionTag*, RE::BSFixedString a_menu, RE::BSFixedString a_key, RE::BSFixedString a_value)
		{
			Runtime::Get().SetData(a_menu.c_str(), a_key.c_str(), std::string(a_value.c_str()), "papyrus");
		}
		void SetNumber(RE::StaticFunctionTag*, RE::BSFixedString a_menu, RE::BSFixedString a_key, float a_value)
		{
			Runtime::Get().SetData(a_menu.c_str(), a_key.c_str(), static_cast<double>(a_value), "papyrus");
		}
		void SetBool(RE::StaticFunctionTag*, RE::BSFixedString a_menu, RE::BSFixedString a_key, bool a_value)
		{
			Runtime::Get().SetData(a_menu.c_str(), a_key.c_str(), a_value, "papyrus");
		}
	}

	bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm)
	{
		a_vm->RegisterFunction("GetVersion", kScript, GetVersion);
		a_vm->RegisterFunction("GetVersionString", kScript, GetVersionString);
		a_vm->RegisterFunction("IsDevMode", kScript, IsDevMode);
		a_vm->RegisterFunction("IsMenuRegistered", kScript, IsMenuRegistered);
		a_vm->RegisterFunction("IsMenuOpen", kScript, IsMenuOpen);
		a_vm->RegisterFunction("Open", kScript, Open);
		a_vm->RegisterFunction("Close", kScript, Close);
		a_vm->RegisterFunction("SetString", kScript, SetString);
		a_vm->RegisterFunction("SetNumber", kScript, SetNumber);
		a_vm->RegisterFunction("SetBool", kScript, SetBool);
		log::info("Papyrus: 10 native functions registered on {} (MakeVersion is plain Papyrus)", kScript);
		return true;
	}
}
