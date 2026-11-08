// Read-only integration-test oracle. Actual KWin-managed windows, never fake
// titles/process liveness. The receiver checks the actual KWin D-Bus sender.
function snapshot() {
    var windows = workspace.windowList();
    var result = [];
    for (var i = 0; i < windows.length && result.length < 32; ++i) {
        var w = windows[i], r = w.frameGeometry;
        result.push({caption:String(w.caption).slice(0,128), resourceClass:String(w.resourceClass).slice(0,128),
                     pid:w.pid, minimized:w.minimized, hidden:w.hidden === true,
                     normal:w.normalWindow, dialog:w.dialog, active:w.active,
                     x:r.x, y:r.y, width:r.width, height:r.height});
    }
    callDBus('org.linuxrunner.TestWindowWatch', '/WindowWatch',
             'org.linuxrunner.TestWindowWatch', 'snapshot', JSON.stringify(result));
}
function attach(w) {
    w.captionChanged.connect(snapshot);
    w.frameGeometryChanged.connect(snapshot);
    w.minimizedChanged.connect(snapshot);
    snapshot();
}
var initial = workspace.windowList();
for (var i = 0; i < initial.length; ++i) attach(initial[i]);
workspace.windowAdded.connect(attach);
workspace.windowRemoved.connect(snapshot);
workspace.windowActivated.connect(snapshot);
snapshot();
