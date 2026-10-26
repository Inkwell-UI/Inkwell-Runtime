// The SKSE co-save side of the saved variables (S5). The record codec and the store are in src/core/Saved.*.
#include "Saved.h"

namespace inkwell
{
	namespace
	{
		void OnRevert(SKSE::SerializationInterface*)
		{
			// new game or before a load: the loaded save brings its own values
			Runtime::Get().saved.Clear();
			log::info("co-save: revert, saved variables cleared");
		}

		void OnSave(SKSE::SerializationInterface* a_intfc)
		{
			try {
				int records = 0, failed = 0;
				for (const auto& [menu, vars] : Runtime::Get().saved.Snapshot()) {
					if (vars.empty()) continue;
					const auto buf = EncodeSavedRecord(menu, vars);
					if (a_intfc->OpenRecord(kCosaveRecordSaved, kCosaveRecordVersion) && a_intfc->WriteRecordData(buf.data(), static_cast<std::uint32_t>(buf.size()))) {
						++records;
						log::debug("co-save: wrote SVAR v{} for '{}': {} variable(s), {} bytes", kCosaveRecordVersion, menu, vars.size(), buf.size());
					} else {
						++failed;
						log::error("co-save: could not write the record for '{}'", menu);
					}
				}
				log::info("co-save: saved {} menu record(s), {} failed", records, failed);
			} catch (const std::exception& e) {
				log::error("co-save: save failed: {}", e.what());
			} catch (...) {
				log::error("co-save: save failed: unknown exception");
			}
		}

		void OnLoad(SKSE::SerializationInterface* a_intfc)
		{
			try {
				auto& store = Runtime::Get().saved;
				store.Clear();  // SKSE calls revert first; this keeps a load self-contained
				std::uint32_t type = 0, version = 0, length = 0;
				int           loaded = 0, skipped = 0;
				while (a_intfc->GetNextRecordInfo(type, version, length)) {
					if (type != kCosaveRecordSaved) {
						log::debug("co-save: record type 0x{:08X} is not ours; left to SKSE", type);
						continue;
					}
					if (version != kCosaveRecordVersion) {
						++skipped;
						log::error("co-save: SVAR record version {} is not understood by this runtime (reads {}); skipped, those saved values are lost for this session", version, kCosaveRecordVersion);
						continue;
					}
					if (length > kSavedRecordMaxBytes) {
						++skipped;
						log::error("co-save: SVAR record of {} bytes is over the limit ({}); skipped", length, kSavedRecordMaxBytes);
						continue;
					}
					std::vector<std::uint8_t> buf(length);
					if (a_intfc->ReadRecordData(buf.data(), length) != length) {
						++skipped;
						log::error("co-save: SVAR record could not be read completely ({} bytes expected); skipped", length);
						continue;
					}
					std::string err;
					auto        rec = DecodeSavedRecord(buf.data(), buf.size(), err);
					if (!rec) {
						++skipped;
						log::error("co-save: SVAR record is damaged ({}); skipped", err);
						continue;
					}
					if (!err.empty()) log::warn("co-save: SVAR record for '{}': {}", rec->menu, err);
					log::info("co-save: loaded '{}': {} variable(s)", rec->menu, rec->vars.size());
					store.Replace(MenuNameKey(rec->menu), std::move(rec->vars));
					++loaded;
				}
				log::info("co-save: load done, {} menu record(s) loaded, {} skipped", loaded, skipped);
			} catch (const std::exception& e) {
				log::error("co-save: load failed: {}", e.what());
			} catch (...) {
				log::error("co-save: load failed: unknown exception");
			}
		}
	}

	void InitSavedState()
	{
		const auto* s = SKSE::GetSerializationInterface();
		s->SetUniqueID(kCosaveUniqueId);
		s->SetRevertCallback(OnRevert);
		s->SetSaveCallback(OnSave);
		s->SetLoadCallback(OnLoad);
		log::info("co-save: callbacks registered (unique id 0x{:08X}, record SVAR v{})", kCosaveUniqueId, kCosaveRecordVersion);
	}
}
