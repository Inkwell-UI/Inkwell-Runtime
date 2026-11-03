#pragma once
#include "PCH.h"

namespace inkwell
{
	// The game's own prompt art (S-M): copies Interface/<name>.swf from the game's own resources (a loose file or the BSA) into
	// Data/Interface/Inkwell/_vanilla/<name>.swf, once, so a custom menu in its sub-folder can load it by "../_vanilla/<name>.swf" (a loose file
	// resolves ".."; an archive entry does not). The copy lives on the player's machine only; nothing of it is shipped. Logs every step.
	void ExtractVanillaInterface();
}
