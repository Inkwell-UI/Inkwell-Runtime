;; original stub (declarations only), written for Inkwell tests; not a Bethesda or SKSE file.
Scriptname ScriptObject Native Hidden
Function RegisterForModEvent(string eventName, string callbackName) native
Function UnregisterForModEvent(string eventName) native
Function SendModEvent(string eventName, string strArg = "", float numArg = 0.0) native
