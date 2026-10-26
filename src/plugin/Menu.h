// InkwellMenu: the one RE::IMenu subclass behind every registered menu (one template slot per menu).
#pragma once
#include "Runtime.h"

#include <chrono>

namespace inkwell
{
	class InkwellMenu : public RE::IMenu
	{
	public:
		explicit InkwellMenu(MenuEntry& a_entry);
		~InkwellMenu() override;

		void                   PostCreate() override;
		RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& a_message) override;
		void                   AdvanceMovie(float a_interval, std::uint32_t a_currentTime) override;

		// UI thread only (Runtime queues these through SKSE's UI task interface)
		void DeliverMessage(const DataEntry& a_entry);
		void DeliverModEvent(const std::string& a_name, const std::string& a_strArg, float a_numArg);
		void DeliverInput(const std::string& a_action);

		MenuEntry& Entry() { return _entry; }
		bool       IsOpenPhase() const { return _phase == Phase::kOpen; }

	private:
		enum class Phase { kLoading, kWaitGlue, kOpen, kClosing, kFailed };

		void BeginOpen(bool a_glueReady, long long a_waitedMs);
		void BeginClose(const char* a_why);

		MenuEntry&                            _entry;
		std::string                           _name;
		Phase                                 _phase{ Phase::kLoading };
		bool                                  _openedBroadcast{ false };
		std::chrono::steady_clock::time_point _created;
	};

	/// Registers `a_name` with the engine in template slot `a_slot`. False if the slot is out of range.
	bool RegisterSlot(std::size_t a_slot, const std::string& a_name);

	/// The open InkwellMenu with this registered name, or null. Only valid for names that came from the registry.
	InkwellMenu* FindOpenMenu(std::string_view a_name);
}
