#include "Messaging.h"

#include "InkwellAPI.h"

#include <cstring>

namespace inkwell
{
	namespace
	{
		bool I_OpenMenu(const char* m) { return m && Runtime::Get().OpenMenu(m, "messaging", false); }
		bool I_CloseMenu(const char* m) { return m && Runtime::Get().CloseMenu(m, "messaging"); }
		bool I_IsMenuOpen(const char* m) { return m && Runtime::Get().IsMenuOpen(m); }
		void I_SetString(const char* menu, const char* key, const char* value)
		{
			if (menu && key) Runtime::Get().SetData(menu, key, std::string(value ? value : ""), "messaging");
		}
		void I_SetNumber(const char* menu, const char* key, double value)
		{
			if (menu && key) Runtime::Get().SetData(menu, key, value, "messaging");
		}
		void I_SetBool(const char* menu, const char* key, bool value)
		{
			if (menu && key) Runtime::Get().SetData(menu, key, value, "messaging");
		}

		bool I_RegisterFunction(const char* name, InkwellFunction fn, void* ctx)
		{
			if (!name || !fn) {
				log::warn("messaging RegisterFunction: null name or function");
				return false;
			}
			const std::string n = name;
			const auto        err = Runtime::Get().functions.Register(n, [fn, ctx](const std::string& arg) {
                std::string buf(4096, '\0');
                fn(arg.c_str(), buf.data(), static_cast<std::uint32_t>(buf.size()), ctx);
                buf.back() = '\0';
                buf.resize(std::strlen(buf.c_str()));
                return buf;
            });
			if (!err.empty()) {
				log::warn("messaging RegisterFunction('{}') refused: {}", n, err);
				return false;
			}
			log::info("messaging RegisterFunction('{}') accepted", n);
			return true;
		}

		const InkwellInterfaceV1& Interface()
		{
			static const InkwellInterfaceV1 i = {
				sizeof(InkwellInterfaceV1), INKWELL_INTERFACE_VERSION, Pack(kRuntimeVersion),
				&I_OpenMenu, &I_CloseMenu, &I_IsMenuOpen, &I_SetString, &I_SetNumber, &I_SetBool, &I_RegisterFunction
			};
			return i;
		}

		// Messages from other plugins (any sender other than SKSE itself)
		void OnPluginMessage(SKSE::MessagingInterface::Message* a_msg)
		{
			if (!a_msg || !a_msg->sender || std::string_view(a_msg->sender) == "SKSE") return;
			if (a_msg->type == INKWELL_MSG_GET_INTERFACE) {
				log::info("messaging: '{}' asked for the interface; answering", a_msg->sender);
				const auto& i = Interface();
				SKSE::GetMessagingInterface()->Dispatch(INKWELL_MSG_INTERFACE_READY, const_cast<InkwellInterfaceV1*>(&i), sizeof(i), a_msg->sender);
			}
		}
	}

	void InitMessaging()
	{
		// nullptr = every sender (unverified in game); the callback ignores SKSE's own lifecycle messages.
		const bool ok = SKSE::GetMessagingInterface()->RegisterListener(nullptr, OnPluginMessage);
		log::info("messaging: listener for other plugins registered: {}", ok);

		// built-in Call Runtime functions (O1)
		auto& ft = Runtime::Get().functions;
		ft.Register("inkwell.version", [](const std::string&) { return ToString(kRuntimeVersion); });
		ft.Register("inkwell.ismenuopen", [](const std::string& a) { return std::string(Runtime::Get().IsMenuOpen(a) ? "1" : "0"); });
		ft.Register("inkwell.isdevmode", [](const std::string&) { return std::string("0"); });
	}

	void DispatchInterfaceReady()
	{
		const auto& i = Interface();
		const bool  ok = SKSE::GetMessagingInterface()->Dispatch(INKWELL_MSG_INTERFACE_READY, const_cast<InkwellInterfaceV1*>(&i), sizeof(i), nullptr);
		log::info("messaging: kInterfaceReady broadcast (interface v{}, size {}): {}", i.interfaceVersion, i.size, ok);
	}

	void BroadcastMenuMessage(const std::string& a_menu, bool a_opened)
	{
		SKSE::GetMessagingInterface()->Dispatch(a_opened ? INKWELL_MSG_MENU_OPENED : INKWELL_MSG_MENU_CLOSED, const_cast<char*>(a_menu.c_str()), static_cast<std::uint32_t>(a_menu.size() + 1), nullptr);
	}
}
