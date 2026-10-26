#pragma once
#include "Runtime.h"

namespace inkwell
{
	/// SKSE::PapyrusInterface callback: registers the Inkwell_API natives (S4.1).
	bool RegisterPapyrus(RE::BSScript::IVirtualMachine* a_vm);
}
