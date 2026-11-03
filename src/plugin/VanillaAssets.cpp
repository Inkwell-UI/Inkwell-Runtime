#include "VanillaAssets.h"

#include <filesystem>
#include <fstream>
#include <vector>

namespace inkwell
{
	namespace
	{
		namespace fs = std::filesystem;
		using namespace SKSE;

		// Reads a whole file through the engine's resource system (loose files and archives alike). Empty = not found or unreadable.
		std::vector<char> ReadResource(const std::string& path)
		{
			std::vector<char> out;
			RE::BSResourceNiBinaryStream stream(path);
			if (!stream.good()) return out;
			// byte by byte (the public API reports only whole reads): fine for the small menu SWFs this spike copies
			char c;
			while (stream.get(c) && out.size() < (32u << 20)) out.push_back(c);
			return out;
		}
	}

	void ExtractVanillaInterface()
	{
		// keep in step with ART_FILES in Inkwell-Editor src/main/swf/promptArt.ts
		static const char* const kNames[] = { "textentry", "creationclubmenu", "sharedcomponents" };
		const fs::path dir = fs::path("Data") / "Interface" / "Inkwell" / "_vanilla";
		std::error_code ec;
		fs::create_directories(dir, ec);
		if (ec) {
			log::warn("vanilla assets: cannot create {}: {}", dir.string(), ec.message());
			return;
		}
		for (const char* n : kNames) {
			const fs::path target = dir / (std::string(n) + ".swf");
			if (fs::exists(target, ec) && fs::file_size(target, ec) > 0) {
				log::info("vanilla assets: {} already cached ({} bytes)", target.string(), fs::file_size(target, ec));
				continue;
			}
			const auto data = ReadResource(std::string("Interface/") + n + ".swf");
			if (data.empty()) {
				log::warn("vanilla assets: Interface/{}.swf could not be read from the game's resources", n);
				continue;
			}
			std::ofstream f(target, std::ios::binary | std::ios::trunc);
			f.write(data.data(), static_cast<std::streamsize>(data.size()));
			f.close();
			log::info("vanilla assets: wrote {} ({} bytes)", target.string(), data.size());
		}
	}
}
