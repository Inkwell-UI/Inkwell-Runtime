// A message produced by a pure parser; the plugin decides how to log it. Keeps the core free of spdlog and the engine.
#pragma once
#include <string>

namespace inkwell
{
	struct Problem
	{
		enum class Level { kDebug, kWarn, kError };
		Level       level{ Level::kError };
		std::string message;
	};
}
