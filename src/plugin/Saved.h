#pragma once
#include "Runtime.h"

namespace inkwell
{
	/// Registers the SKSE co-save callbacks (unique id 'INKW', record 'SVAR' v1, S5). Call at plugin load.
	void InitSavedState();
}
