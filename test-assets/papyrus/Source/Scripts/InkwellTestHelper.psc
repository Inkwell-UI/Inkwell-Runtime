Scriptname InkwellTestHelper Hidden
{Test helper for docs/test-notes.md. Call it from a script that is attached to a quest: InkwellTestHelper.SendOpenEvent("InkwellTest_Open")
Not part of the runtime. Needs SKSE (SendModEvent).}

; Sends an SKSE mod event from the player. A menu whose triggers.modEvent equals eventName opens; an open menu gets it as On Mod Event.
Function SendOpenEvent(String eventName, String strArg = "hello", Float numArg = 7.0) Global
	Game.GetPlayer().SendModEvent(eventName, strArg, numArg)
	Debug.Notification("Inkwell test: sent mod event " + eventName)
EndFunction

; Shows the runtime version and whether InkwellTest_A is registered as a notification: InkwellTestHelper.ReportVersion() from an attached script
Function ReportVersion() Global
	Debug.Notification("Inkwell Runtime " + Inkwell_API.GetVersionString() + " packed " + Inkwell_API.GetVersion() + " registered A: " + Inkwell_API.IsMenuRegistered("InkwellTest_A"))
EndFunction
