// Frame-1 script of Interface/Inkwell/SystemMenuInjector.swf (compiled by the editor: `yarn compile:system-injector`).
// The runtime creates the clip `_root.InkwellSystem` in the Journal Menu, sets _global.InkwellSystemMenus ("label<TAB>menu<LF>" per entry) and loads
// this SWF into the clip, so `this._parent` is the Journal root. It adds one entry per line under Load on the System page; picking one sends the SKSE
// mod event Inkwell_SystemMenu with the menu name, and the runtime closes the Journal and opens the menu. Same approach as Achievement Injector.
var journal = this._parent;
var systemPage = journal.QuestJournalFader.Menu_mc.SystemFader.Page_mc;
var categoryList = systemPage.CategoryList_mc.List_mc;
var entries = [];
var lines = String(_global.InkwellSystemMenus).split("\n");
for (var i = 0; i < lines.length; i++) {
    var cols = lines[i].split("\t");
    if (cols.length == 2) {
        entries.push({ text: cols[0], inkwellMenu: cols[1] });
    }
}

function hasInkwell(list) {
    for (var i = 0; i < list.length; i++) {
        if (list[i].inkwellMenu != undefined) {
            return true;
        }
    }
    return false;
}

categoryList.__InvalidateData = categoryList.InvalidateData;
categoryList.InvalidateData = function() {
    // The game may rebuild entryList at any time, so insert whenever our entries are missing (not once)
    var list = categoryList.entryList;
    if (!hasInkwell(list)) {
        var at = 0;
        for (var i = 0; i < list.length; i++) {
            if (list[i].text === "$LOAD") {
                at = i + 1;
                break;
            }
        }
        categoryList.entryList = list.slice(0, at).concat(entries, list.slice(at));
        // Newer SystemPage dispatches on IDX_* positions it computed before our entries existed; recompute
        if (typeof systemPage.UpdateIndices === "function") {
            systemPage.UpdateIndices();
        }
    }
    categoryList.__InvalidateData();
};
categoryList.InvalidateData();

systemPage.__onCategoryButtonPress = systemPage.onCategoryButtonPress;
systemPage.onCategoryButtonPress = function(event) {
    if (event.entry.inkwellMenu != undefined) {
        _global.skse.SendModEvent("Inkwell_SystemMenu", event.entry.inkwellMenu);
        return;
    }
    // Older SystemPage dispatches on fixed vanilla positions: hide our entries above the clicked one from the index
    if (typeof systemPage.UpdateIndices !== "function") {
        var list = categoryList.entryList;
        var above = 0;
        for (var i = 0; i < event.index && i < list.length; i++) {
            if (list[i].inkwellMenu != undefined) {
                above++;
            }
        }
        event.index -= above;
    }
    systemPage.__onCategoryButtonPress(event);
};
