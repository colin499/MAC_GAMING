import AppKit

/// Collection view that turns Return/Space/Delete into library actions and leaves arrows to AppKit.
final class LibraryCollectionView: NSCollectionView {
    var onPrimary: (() -> Void)?
    var onToggleDetail: (() -> Void)?
    var onDelete: (() -> Void)?
    override func keyDown(with event: NSEvent) {
        switch event.keyCode {
        case 36, 76: onPrimary?()
        case 49: onToggleDetail?()
        case 51, 117: onDelete?()
        default: super.keyDown(with: event)
        }
    }
    override var acceptsFirstResponder: Bool { true }
}

/// Free-disk bar with the hovered or selected game's size drawn as a lighter segment.
final class DiskGaugeView: NSView {
    var free: Int64 = 0 { didSet { needsDisplay = true } }
    var total: Int64 = 0 { didSet { needsDisplay = true } }
    var highlight: Int64 = 0 { didSet { needsDisplay = true } }
    override var intrinsicContentSize: NSSize { NSSize(width: 170, height: 30) }
    override func draw(_ dirtyRect: NSRect) {
        guard total > 0 else { return }
        let bar = NSRect(x: 0, y: 17, width: bounds.width, height: 7)
        NSColor.white.withAlphaComponent(0.10).setFill(); NSBezierPath(roundedRect: bar, xRadius: 3.5, yRadius: 3.5).fill()
        let used = CGFloat(Double(total - free) / Double(total)) * bar.width
        Theme.textSecondary.withAlphaComponent(0.55).setFill()
        NSBezierPath(roundedRect: NSRect(x: 0, y: bar.minY, width: max(3, used), height: bar.height), xRadius: 3.5, yRadius: 3.5).fill()
        if highlight > 0 {
            let w = min(bar.width - used, CGFloat(Double(highlight) / Double(total)) * bar.width)
            Theme.accent.setFill(); NSBezierPath(rect: NSRect(x: used, y: bar.minY, width: max(2, w), height: bar.height)).fill()
        }
        let text = Format.gb(free) + " free of " + Format.gb(total) + (highlight > 0 ? "  (+" + Format.gb(highlight) + ")" : "")
        (text as NSString).draw(at: NSPoint(x: 0, y: 1), withAttributes: [.font: Theme.font(10), .foregroundColor: Theme.textSecondary])
    }
}

/// The error strip under the top bar.
final class ErrorBannerView: NSView {
    private let message = makeLabel("", size: 12, weight: .medium, lines: 2)
    private let details = NSTextField(wrappingLabelWithString: "")
    private let detailsButton = NSButton(title: "Details", target: nil, action: nil)
    var onShowLog: (() -> Void)?
    var onDismiss: (() -> Void)?
    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = Theme.red.withAlphaComponent(0.18).cgColor
        translatesAutoresizingMaskIntoConstraints = false
        details.font = NSFont.monospacedSystemFont(ofSize: 10, weight: .regular)
        details.textColor = Theme.textSecondary
        details.isHidden = true
        details.maximumNumberOfLines = 8
        details.translatesAutoresizingMaskIntoConstraints = false
        detailsButton.bezelStyle = .inline; detailsButton.controlSize = .small
        detailsButton.target = self; detailsButton.action = #selector(toggle)
        let log = NSButton(title: "Show log", target: self, action: #selector(showLog)); log.bezelStyle = .inline; log.controlSize = .small
        let close = NSButton(title: "Dismiss", target: self, action: #selector(dismiss)); close.bezelStyle = .inline; close.controlSize = .small
        let row = NSStackView(views: [message, detailsButton, log, close]); row.spacing = 10
        row.translatesAutoresizingMaskIntoConstraints = false
        message.setContentHuggingPriority(.defaultLow, for: .horizontal)
        let col = NSStackView(views: [row, details]); col.orientation = .vertical; col.alignment = .leading; col.spacing = 4
        col.translatesAutoresizingMaskIntoConstraints = false
        addSubview(col)
        NSLayoutConstraint.activate([col.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 16), col.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -16),
                                     col.topAnchor.constraint(equalTo: topAnchor, constant: 6), col.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -6),
                                     row.trailingAnchor.constraint(equalTo: col.trailingAnchor)])
        isHidden = true
    }
    required init?(coder: NSCoder) { fatalError() }
    func show(_ msg: String, details d: String) {
        message.stringValue = msg; details.stringValue = d.trimmingCharacters(in: .whitespacesAndNewlines)
        detailsButton.isHidden = details.stringValue.isEmpty; details.isHidden = true
        isHidden = false
    }
    @objc private func toggle() { details.isHidden.toggle() }
    @objc private func showLog() { onShowLog?() }
    @objc private func dismiss() { isHidden = true; onDismiss?() }
}

final class LibraryViewController: NSViewController, NSCollectionViewDataSource, NSCollectionViewDelegateFlowLayout, GameTileDelegate, NSSearchFieldDelegate {
    let backend = Backend.shared
    let search = NSSearchField()
    let filterControl = NSSegmentedControl(labels: ["All", "Installed", "Not installed"], trackingMode: .selectOne, target: nil, action: nil)
    let sortPopup = NSPopUpButton()
    let gauge = DiskGaugeView()
    let steamButton = NSButton(title: "Open Steam", target: nil, action: nil)
    let refreshButton = NSButton(title: "↻", target: nil, action: nil)
    let spinner = NSProgressIndicator()
    let banner = ErrorBannerView()
    let scroll = NSScrollView()
    let collection = LibraryCollectionView()
    let detail = DetailPaneView(frame: .zero)
    let statusLeft = makeLabel("", size: 11, color: Theme.textSecondary)
    let statusCenter = makeLabel("", size: 11, color: Theme.textSecondary)
    let statusRight = makeLabel("", size: 11, color: Theme.textTertiary)
    let flightSwitch = NSSwitch()
    let emptyLabel = makeLabel("", size: 14, color: Theme.textTertiary, lines: 4)
    var visible: [Game] = []
    var signatures: [String] = []
    var selectedAppid: String?
    var filter: LibraryFilter = .installedFirstDefault
    var sort: LibrarySort = .installedFirst
    var statusTimer: Timer?

    override func loadView() {
        let root = NSView(frame: NSRect(x: 0, y: 0, width: 1100, height: 720))
        root.wantsLayer = true
        root.layer?.backgroundColor = Theme.background.cgColor
        view = root

        // top bar
        let top = NSView(); top.wantsLayer = true; top.layer?.backgroundColor = Theme.panel.cgColor
        top.translatesAutoresizingMaskIntoConstraints = false
        search.placeholderString = "Search games"
        search.sendsSearchStringImmediately = true
        search.delegate = self
        search.target = self; search.action = #selector(searchChanged)
        search.translatesAutoresizingMaskIntoConstraints = false
        filterControl.target = self; filterControl.action = #selector(filterChanged)
        filterControl.selectedSegment = 0
        sortPopup.addItems(withTitles: ["Installed first", "Name", "Size", "Last played"])
        sortPopup.target = self; sortPopup.action = #selector(sortChanged)
        steamButton.bezelStyle = .rounded; steamButton.target = self; steamButton.action = #selector(steamTapped)
        refreshButton.bezelStyle = .rounded; refreshButton.target = self; refreshButton.action = #selector(refreshTapped)
        refreshButton.toolTip = "Refresh (⌘R)"
        spinner.style = .spinning; spinner.controlSize = .small; spinner.isDisplayedWhenStopped = false
        gauge.translatesAutoresizingMaskIntoConstraints = false
        let spacer = NSView(); spacer.translatesAutoresizingMaskIntoConstraints = false
        spacer.setContentHuggingPriority(.init(1), for: .horizontal)
        let bar = NSStackView(views: [search, filterControl, sortPopup, spacer, gauge, steamButton, refreshButton, spinner])
        bar.spacing = 12; bar.alignment = .centerY
        bar.translatesAutoresizingMaskIntoConstraints = false
        top.addSubview(bar)
        NSLayoutConstraint.activate([
            top.heightAnchor.constraint(equalToConstant: 52),
            search.widthAnchor.constraint(equalToConstant: 240),
            spacer.widthAnchor.constraint(greaterThanOrEqualToConstant: 8),
            bar.leadingAnchor.constraint(equalTo: top.leadingAnchor, constant: 84),   // room for the traffic lights
            bar.trailingAnchor.constraint(equalTo: top.trailingAnchor, constant: -16),
            bar.centerYAnchor.constraint(equalTo: top.centerYAnchor),
        ])

        // grid
        let layout = NSCollectionViewFlowLayout()
        layout.itemSize = NSSize(width: Theme.tileWidth, height: Theme.tileHeight)
        layout.minimumInteritemSpacing = 18; layout.minimumLineSpacing = 22
        layout.sectionInset = NSEdgeInsets(top: 20, left: 24, bottom: 24, right: 24)
        collection.collectionViewLayout = layout
        collection.register(GameTileItem.self, forItemWithIdentifier: GameTileItem.identifier)
        collection.dataSource = self; collection.delegate = self
        collection.isSelectable = true; collection.allowsEmptySelection = true; collection.allowsMultipleSelection = false
        collection.backgroundColors = [Theme.background]
        collection.onPrimary = { [weak self] in self?.primaryForSelection() }
        collection.onToggleDetail = { [weak self] in self?.toggleDetail() }
        collection.onDelete = { [weak self] in if let g = self?.selectedGame(), g.installed { self?.confirmRemove(g) } }
        scroll.documentView = collection
        scroll.hasVerticalScroller = true; scroll.drawsBackground = false; scroll.borderType = .noBorder
        scroll.translatesAutoresizingMaskIntoConstraints = false
        emptyLabel.alignment = .center
        emptyLabel.isHidden = true

        detail.onPrimary = { [weak self] g in self?.primary(g) }
        detail.onRemove = { [weak self] g in self?.confirmRemove(g) }
        let divider = NSView(); divider.wantsLayer = true; divider.layer?.backgroundColor = Theme.separator.cgColor
        divider.translatesAutoresizingMaskIntoConstraints = false
        let middle = NSStackView(views: [scroll, divider, detail]); middle.spacing = 0; middle.orientation = .horizontal
        middle.translatesAutoresizingMaskIntoConstraints = false
        middle.distribution = .fill
        NSLayoutConstraint.activate([detail.widthAnchor.constraint(equalToConstant: Theme.detailWidth), divider.widthAnchor.constraint(equalToConstant: 1)])

        // status bar
        let status = NSView(); status.wantsLayer = true; status.layer?.backgroundColor = Theme.panel.cgColor
        status.translatesAutoresizingMaskIntoConstraints = false
        flightSwitch.controlSize = .mini; flightSwitch.target = self; flightSwitch.action = #selector(flightToggled)
        let flightLabel = makeLabel("Flight mode", size: 11, color: Theme.textSecondary)
        flightLabel.toolTip = "Steam offline mode for the plane: closes Steam and flips its remembered offline flag"
        let sp1 = NSView(), sp2 = NSView()
        [sp1, sp2].forEach { $0.setContentHuggingPriority(.init(1), for: .horizontal) }
        statusCenter.alignment = .center
        let srow = NSStackView(views: [statusLeft, flightSwitch, flightLabel, sp1, statusCenter, sp2, statusRight]); srow.spacing = 10
        srow.translatesAutoresizingMaskIntoConstraints = false
        status.addSubview(srow)
        NSLayoutConstraint.activate([status.heightAnchor.constraint(equalToConstant: 28),
                                     srow.leadingAnchor.constraint(equalTo: status.leadingAnchor, constant: 16), srow.trailingAnchor.constraint(equalTo: status.trailingAnchor, constant: -16),
                                     srow.centerYAnchor.constraint(equalTo: status.centerYAnchor),
                                     sp1.widthAnchor.constraint(equalTo: sp2.widthAnchor)])

        root.addSubview(top); root.addSubview(banner); root.addSubview(middle); root.addSubview(status); root.addSubview(emptyLabel)
        NSLayoutConstraint.activate([
            top.topAnchor.constraint(equalTo: root.topAnchor), top.leadingAnchor.constraint(equalTo: root.leadingAnchor), top.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            banner.topAnchor.constraint(equalTo: top.bottomAnchor), banner.leadingAnchor.constraint(equalTo: root.leadingAnchor), banner.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            middle.topAnchor.constraint(equalTo: banner.bottomAnchor), middle.leadingAnchor.constraint(equalTo: root.leadingAnchor), middle.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            middle.bottomAnchor.constraint(equalTo: status.topAnchor),
            status.leadingAnchor.constraint(equalTo: root.leadingAnchor), status.trailingAnchor.constraint(equalTo: root.trailingAnchor), status.bottomAnchor.constraint(equalTo: root.bottomAnchor),
            emptyLabel.centerXAnchor.constraint(equalTo: scroll.centerXAnchor), emptyLabel.centerYAnchor.constraint(equalTo: scroll.centerYAnchor),
            emptyLabel.widthAnchor.constraint(lessThanOrEqualToConstant: 420),
        ])
        banner.onShowLog = { [weak self] in self?.showLog() }
        banner.onDismiss = { [weak self] in self?.backend.clearError() }

        backend.onUpdate = { [weak self] in self?.reload() }
        backend.onError = { [weak self] m, d in self?.banner.show(m, details: d) }
        backend.onBusyChange = { [weak self] in
            guard let self = self else { return }
            if self.backend.busy > 0 { self.spinner.startAnimation(nil) } else { self.spinner.stopAnimation(nil) }
        }
        CoverStore.shared.onLoaded = { [weak self] appid in self?.reloadTile(appid) }
        statusTimer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { [weak self] _ in self?.updateStatus() }
    }

    override func viewDidAppear() {
        super.viewDidAppear()
        view.window?.makeFirstResponder(collection)
    }

    // MARK: data

    func reload() {
        guard let lib = backend.library else {
            visible = []; collection.reloadData(); updateEmpty(); updateStatus(); return
        }
        let list = Sorting.apply(lib.games, filter: filter, sort: sort, search: search.stringValue)
        let sigs = list.map { signature($0) }
        if list.map({ $0.appid }) == visible.map({ $0.appid }) {
            var changed = IndexSet()
            for (i, s) in sigs.enumerated() where i >= signatures.count || signatures[i] != s { changed.insert(i) }
            visible = list; signatures = sigs
            if !changed.isEmpty { collection.reloadItems(at: Set(changed.map { IndexPath(item: $0, section: 0) })) }
        } else {
            visible = list; signatures = sigs
            collection.reloadData()
            if let sel = selectedAppid, let i = visible.firstIndex(where: { $0.appid == sel }) {
                collection.selectItems(at: [IndexPath(item: i, section: 0)], scrollPosition: [])
            }
        }
        if let sel = selectedAppid {
            detail.show(visible.first { $0.appid == sel } ?? lib.games.first { $0.appid == sel })
        } else { detail.show(nil) }
        gauge.free = lib.disk.free; gauge.total = lib.disk.total
        steamButton.title = lib.steam.running ? "Quit Steam" : "Open Steam"
        flightSwitch.state = lib.steam.offline ? .on : .off
        updateEmpty(); updateStatus()
        if let e = backend.lastError { banner.show(e.message, details: e.details) } else { banner.isHidden = true }
    }

    private func signature(_ g: Game) -> String {
        let p = backend.pending[g.appid].map { "\($0.kind)-\($0.message)" } ?? ""
        return "\(g.installed)\(g.ready)\(g.downloading)\(g.progress)\(g.size)\(g.isRunning)\(g.needs_update)\(g.compat.verdict)\(p)\(backend.failed[g.appid] ?? "")\(g.covers.capsule ?? "")"
    }

    private func reloadTile(_ appid: String) {
        guard let i = visible.firstIndex(where: { $0.appid == appid }) else { return }
        collection.reloadItems(at: [IndexPath(item: i, section: 0)])
        if selectedAppid == appid { detail.show(visible[i]) }
    }

    private func updateEmpty() {
        let lib = backend.library
        var text: String?
        if lib == nil && backend.lastError != nil { text = "Layover could not load the library.\nSee the message above." }
        else if let l = lib, !(l.layover.engine_ready && l.layover.steam_installed) { text = "Layover is not set up yet.\nOpen Terminal in the MAC_GAMING folder and run:  ./layover setup" }
        else if lib != nil && visible.isEmpty {
            if !search.stringValue.isEmpty { text = "No games match “\(search.stringValue)”" }
            else if filter == .installed { text = "Nothing installed yet. Pick a game and press Install." }
            else if filter == .notInstalled { text = "Everything you own is installed." }
            else { text = "No games found. Sign in to Windows Steam once (Open Steam) so Layover can read your library." }
        } else if lib == nil { text = "Loading…" }
        emptyLabel.stringValue = text ?? ""
        emptyLabel.isHidden = text == nil
    }

    func updateStatus() {
        guard let lib = backend.library else { statusLeft.stringValue = "Loading…"; return }
        let acct = lib.steam.accounts?.first.map { $0.persona ?? $0.account_name ?? "" } ?? ""
        var left = lib.steam.running ? "● Steam running" : "○ Steam off"
        if lib.steam.running, let c = lib.steam.connection { left += " · " + c }
        if lib.steam.offline { left += " · offline mode" }
        if !acct.isEmpty { left += " · " + acct }
        statusLeft.stringValue = left
        statusLeft.textColor = lib.steam.running ? Theme.green : Theme.textSecondary
        let n = lib.games.filter { $0.owned }.count
        var center = "\(n) game\(n == 1 ? "" : "s") · \(lib.counts.installed) installed"
        if lib.counts.free > 0 { center += " · \(lib.counts.free) free" }
        if lib.counts.downloading > 0 { center += " · \(lib.counts.downloading) downloading" }
        statusCenter.stringValue = center
        let ago = backend.lastRefresh.map { Int(Date().timeIntervalSince($0)) } ?? 0
        statusRight.stringValue = "Refreshed \(ago < 3 ? "just now" : "\(ago) s ago") · Layover \(lib.layover.version ?? "")"
        if let e = lib.appcache?.appinfo_error { statusRight.stringValue += " · Steam app info unreadable: \(e)" }
    }

    // MARK: collection view

    func collectionView(_ collectionView: NSCollectionView, numberOfItemsInSection section: Int) -> Int { visible.count }

    func collectionView(_ collectionView: NSCollectionView, itemForRepresentedObjectAt indexPath: IndexPath) -> NSCollectionViewItem {
        let item = collectionView.makeItem(withIdentifier: GameTileItem.identifier, for: indexPath) as! GameTileItem
        let g = visible[indexPath.item]
        item.tile.configure(g, pending: backend.pending[g.appid], failure: backend.failed[g.appid], delegate: self)
        item.tile.selected = collectionView.selectionIndexPaths.contains(indexPath)
        return item
    }

    func collectionView(_ collectionView: NSCollectionView, didSelectItemsAt indexPaths: Set<IndexPath>) {
        guard let i = indexPaths.first?.item, i < visible.count else { return }
        selectedAppid = visible[i].appid
        detail.show(visible[i])
        gauge.highlight = visible[i].installed ? 0 : (visible[i].bytes_to_download ?? 0)
    }
    func collectionView(_ collectionView: NSCollectionView, didDeselectItemsAt indexPaths: Set<IndexPath>) {
        if collectionView.selectionIndexPaths.isEmpty { selectedAppid = nil; detail.show(nil); gauge.highlight = 0 }
    }

    func selectedGame() -> Game? { selectedAppid.flatMap { id in visible.first { $0.appid == id } } }

    // MARK: actions

    func tilePrimaryAction(_ game: Game) { primary(game) }
    func tileHovered(_ game: Game?) { gauge.highlight = game.map { $0.installed ? $0.size : ($0.bytes_to_download ?? 0) } ?? 0 }

    func select(appid: String) {
        selectedAppid = appid
        if let i = visible.firstIndex(where: { $0.appid == appid }) {
            collection.selectItems(at: [IndexPath(item: i, section: 0)], scrollPosition: .centeredVertically)
            detail.show(visible[i])
        } else { detail.show(backend.game(appid)) }
    }

    func primary(_ g: Game) {
        if g.isMac { backend.run(["play", g.appid, "--json"]) { _ in }; return }
        switch g.primary {
        case .play: backend.play(g.appid)
        case .install: backend.install(g.appid)
        case .update: backend.update(g.appid)
        case .downloading, .running: break
        }
    }
    func primaryForSelection() { if let g = selectedGame() { primary(g) } }

    func confirmRemove(_ g: Game) {
        guard g.installed, !g.isMac, let w = view.window else { return }
        let a = NSAlert()
        a.messageText = "Remove \(g.name) from this Mac?"
        a.informativeText = "Frees \(Format.size(g.size)). Steam will ask you to confirm in its window. Saves kept in Steam Cloud stay there."
        a.alertStyle = .warning
        let remove = a.addButton(withTitle: "Remove")
        remove.hasDestructiveAction = true
        a.addButton(withTitle: "Cancel")
        a.beginSheetModal(for: w) { [weak self] r in if r == .alertFirstButtonReturn { self?.backend.remove(g.appid) } }
    }

    @objc func searchChanged() { reload() }
    func controlTextDidChange(_ obj: Notification) { reload() }
    @objc func filterChanged() { filter = LibraryFilter(rawValue: filterControl.selectedSegment) ?? .all; reload() }
    @objc func sortChanged() { sort = LibrarySort(rawValue: sortPopup.indexOfSelectedItem) ?? .installedFirst; reload() }
    @objc func steamTapped() {
        steamButton.isEnabled = false
        let done: (Backend.Result) -> Void = { [weak self] _ in self?.steamButton.isEnabled = true }
        if backend.library?.steam.running == true { backend.quitSteam(completion: done) } else { backend.openSteam(completion: done) }
    }
    @objc func refreshTapped() { backend.refresh() }
    @objc func flightToggled() {
        let offline = flightSwitch.state == .on
        guard let w = view.window else { return }
        let a = NSAlert()
        a.messageText = offline ? "Switch Steam to offline (flight) mode?" : "Switch Steam back online?"
        a.informativeText = offline ? "Steam is closed and will start in offline mode next time. Do this while you still have Wi-Fi; games must have been launched once online."
                                    : "Steam is closed and will start online next time (cloud saves sync at the next launch)."
        a.addButton(withTitle: offline ? "Go offline" : "Go online"); a.addButton(withTitle: "Cancel")
        a.beginSheetModal(for: w) { [weak self] r in
            guard let self = self else { return }
            if r == .alertFirstButtonReturn { self.backend.setFlight(offline: offline) }
            else { self.flightSwitch.state = offline ? .off : .on }
        }
    }
    func toggleDetail() { detail.isHidden.toggle(); detail.superview?.needsLayout = true }
    func focusSearch() { view.window?.makeFirstResponder(search) }
    func setFilter(_ f: LibraryFilter) { filter = f; filterControl.selectedSegment = f.rawValue; reload() }
    func setSort(_ s: LibrarySort) { sort = s; sortPopup.selectItem(at: s.rawValue); reload() }
    func showLog() { NSWorkspace.shared.open(URL(fileURLWithPath: backend.logPath)) }

    override func cancelOperation(_ sender: Any?) {
        if !search.stringValue.isEmpty { search.stringValue = ""; reload() }
        else { collection.deselectAll(nil); selectedAppid = nil; detail.show(nil) }
        view.window?.makeFirstResponder(collection)
    }
}

extension LibraryFilter { static let installedFirstDefault = LibraryFilter.all }
