import AppKit

/// Command-line options (also what the headless snapshot uses).
struct Options {
    var snapshot: String?
    var snapshotDetail: String?
    var mock: String?
    var select: String?
    var search: String?
    var filter: String?
    var sort: String?
    var waitCovers: Double?
    var script: String?
    var python: String?
    var pending: [(String, String)] = []
    var dryRun = false

    static func parse(_ argv: [String]) -> Options {
        var o = Options()
        var i = 1
        func value() -> String? { i += 1; return i < argv.count ? argv[i] : nil }
        while i < argv.count {
            switch argv[i] {
            case "--snapshot": o.snapshot = value()
            case "--snapshot-detail": o.snapshotDetail = value()
            case "--mock": o.mock = value()
            case "--select": o.select = value()
            case "--search": o.search = value()
            case "--filter": o.filter = value()
            case "--sort": o.sort = value()
            case "--wait-covers": o.waitCovers = value().flatMap(Double.init)
            case "--script": o.script = value()
            case "--python": o.python = value()
            case "--pending": if let v = value() { let p = v.split(separator: ":", maxSplits: 1).map(String.init); if p.count == 2 { o.pending.append((p[0], p[1])) } }
            case "--dry-run": o.dryRun = true
            default: break
            }
            i += 1
        }
        return o
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let options: Options
    var window: NSWindow!
    var controller: LibraryViewController!

    init(options: Options) { self.options = options }

    func applicationDidFinishLaunching(_ notification: Notification) {
        let backend = Backend.shared
        backend.locate(scriptFlag: options.script, pythonFlag: options.python)
        backend.mockPath = options.mock
        backend.dryRun = options.dryRun
        if options.snapshot != nil || options.snapshotDetail != nil { CoverStore.shared.downloadsEnabled = options.mock == nil }

        controller = LibraryViewController()
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1100, height: 720), styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
        window.title = "Layover"
        window.titlebarAppearsTransparent = true
        window.titleVisibility = .hidden
        window.appearance = NSAppearance(named: .darkAqua)
        window.backgroundColor = Theme.background
        window.minSize = NSSize(width: 820, height: 560)
        window.contentViewController = controller
        window.delegate = self
        window.isReleasedWhenClosed = false
        if options.snapshot == nil && options.snapshotDetail == nil {
            window.setFrameAutosaveName("LayoverLibrary")
            if !window.setFrameUsingName("LayoverLibrary") { window.center() }
        } else { window.setFrame(NSRect(x: 0, y: 0, width: 1100, height: 720), display: false) }
        buildMenu()
        if let f = options.filter { controller.setFilter(f == "installed" ? .installed : (f == "missing" || f == "not-installed" ? .notInstalled : .all)) }
        if let s = options.sort { controller.setSort(["name": .name, "size": .size, "played": .lastPlayed].first { $0.key == s }?.value ?? .installedFirst) }
        if let q = options.search { controller.search.stringValue = q }
        backend.appendLog("LayoverApp started (\(options.mock == nil ? "live" : "mock")) script=\(backend.script) python=\(backend.python)")
        backend.refresh { [self] in
            for (appid, kind) in options.pending {
                let k: PendingAction.Kind = kind == "remove" ? .remove : (kind == "play" ? .play : (kind == "update" ? .update : .install))
                backend.injectPending(appid, kind: k)
            }
            controller.reload()
            if let s = options.select { controller.select(appid: s) }
            if options.snapshot != nil || options.snapshotDetail != nil {
                Snapshot.run(window: window, controller: controller, options: options)
            }
        }
        if options.snapshot == nil && options.snapshotDetail == nil {
            window.makeKeyAndOrderFront(nil)
            NSApp.activate(ignoringOtherApps: true)
        }
    }

    func applicationDidBecomeActive(_ notification: Notification) {
        if Backend.shared.library != nil { Backend.shared.refresh() }
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    private func buildMenu() {
        let main = NSMenu()
        let appItem = NSMenuItem(); main.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "About Layover", action: #selector(about), keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Quit Layover", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu

        let fileItem = NSMenuItem(); main.addItem(fileItem)
        let file = NSMenu(title: "File")
        file.addItem(withTitle: "Close", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        file.addItem(withTitle: "Show Log", action: #selector(showLog), keyEquivalent: "l")
        fileItem.submenu = file

        let editItem = NSMenuItem(); main.addItem(editItem)
        let edit = NSMenu(title: "Edit")
        edit.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        edit.addItem(.separator())
        edit.addItem(withTitle: "Find", action: #selector(focusSearch), keyEquivalent: "f")
        editItem.submenu = edit

        let viewItem = NSMenuItem(); main.addItem(viewItem)
        let v = NSMenu(title: "View")
        v.addItem(withTitle: "Refresh", action: #selector(refresh), keyEquivalent: "r")
        v.addItem(.separator())
        v.addItem(withTitle: "All Games", action: #selector(filterAll), keyEquivalent: "1")
        v.addItem(withTitle: "Installed", action: #selector(filterInstalled), keyEquivalent: "2")
        v.addItem(withTitle: "Not Installed", action: #selector(filterMissing), keyEquivalent: "3")
        v.addItem(.separator())
        v.addItem(withTitle: "Toggle Details", action: #selector(toggleDetail), keyEquivalent: "d")
        viewItem.submenu = v

        let winItem = NSMenuItem(); main.addItem(winItem)
        let w = NSMenu(title: "Window")
        w.addItem(withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        winItem.submenu = w
        NSApp.mainMenu = main
    }

    @objc func about() {
        NSApp.orderFrontStandardAboutPanel(options: [.applicationName: "Layover", .applicationVersion: Backend.shared.library?.layover.version ?? ""])
    }
    @objc func showLog() { controller.showLog() }
    @objc func focusSearch() { controller.focusSearch() }
    @objc func refresh() { Backend.shared.refresh() }
    @objc func filterAll() { controller.setFilter(.all) }
    @objc func filterInstalled() { controller.setFilter(.installed) }
    @objc func filterMissing() { controller.setFilter(.notInstalled) }
    @objc func toggleDetail() { controller.toggleDetail() }
}

extension Backend {
    /// For snapshots and tests: show a pending state without calling the launcher.
    func injectPending(_ appid: String, kind: PendingAction.Kind) {
        let msg: String
        switch kind {
        case .install: msg = "Steam is asking where to install. Confirm in the Steam window."
        case .update: msg = "Asking Steam to update…"
        case .remove: msg = "Removing… confirm in Steam"
        case .play: msg = "Launching…"
        }
        setPending(appid, PendingAction(kind: kind, started: Date(), message: msg))
    }
}
