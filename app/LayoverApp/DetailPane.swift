import AppKit

/// The right-hand pane: hero art, facts, compatibility, actions and per-game launch settings.
final class DetailPaneView: NSView {
    private let scroll = NSScrollView()
    private let stack = NSStackView()
    private let heroView = AspectFillImageView()
    private let heroShade = NSView()
    private let titleLabel = makeLabel("", size: 20, weight: .bold, lines: 2)
    private let subtitleLabel = makeLabel("", size: 12, color: Theme.textSecondary, lines: 2)
    private let metaGrid = NSGridView(numberOfColumns: 2, rows: 0)
    private let compatPill = BadgeView()
    private let compatLabel = makeLabel("", size: 12, weight: .semibold)
    private let compatReasons = makeLabel("", size: 12, color: Theme.textSecondary, lines: 12)
    private let compatButton = NSButton(title: "Check compatibility", target: nil, action: nil)
    private let primaryButton = NSButton(title: "Play", target: nil, action: nil)
    private let storeButton = NSButton(title: "Steam page", target: nil, action: nil)
    private let removeButton = NSButton(title: "Remove…", target: nil, action: nil)
    private let pendingLabel = makeLabel("", size: 12, color: Theme.blue, lines: 3)
    private let rendererPopup = NSPopUpButton()
    private let fpsPopup = NSPopUpButton()
    private let retinaPopup = NSPopUpButton()
    private let metalfxPopup = NSPopUpButton()
    private let savedLabel = makeLabel("", size: 11, color: Theme.green)
    private let footnote = makeLabel("Applies at the next launch; Steam restarts if needed.", size: 11, color: Theme.textTertiary, lines: 2)
    private let profileButton = NSButton(title: "Reset to recommended", target: nil, action: nil)
    private let notesLabel = makeLabel("", size: 12, color: Theme.textSecondary, lines: 20)
    private let emptyLabel = makeLabel("Select a game", size: 14, color: Theme.textTertiary)
    private var settingsBox: NSStackView!
    private(set) var game: Game?
    var onPrimary: ((Game) -> Void)?
    var onRemove: ((Game) -> Void)?

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = Theme.panel.cgColor
        translatesAutoresizingMaskIntoConstraints = false

        scroll.translatesAutoresizingMaskIntoConstraints = false
        scroll.hasVerticalScroller = true
        scroll.drawsBackground = false
        scroll.borderType = .noBorder
        addSubview(scroll)
        NSLayoutConstraint.activate([scroll.leadingAnchor.constraint(equalTo: leadingAnchor), scroll.trailingAnchor.constraint(equalTo: trailingAnchor),
                                     scroll.topAnchor.constraint(equalTo: topAnchor), scroll.bottomAnchor.constraint(equalTo: bottomAnchor)])
        let doc = FlippedView()
        doc.translatesAutoresizingMaskIntoConstraints = false
        scroll.documentView = doc
        NSLayoutConstraint.activate([doc.leadingAnchor.constraint(equalTo: scroll.contentView.leadingAnchor),
                                     doc.trailingAnchor.constraint(equalTo: scroll.contentView.trailingAnchor),
                                     doc.topAnchor.constraint(equalTo: scroll.contentView.topAnchor)])

        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 10
        stack.edgeInsets = NSEdgeInsets(top: 0, left: 0, bottom: 20, right: 0)
        stack.translatesAutoresizingMaskIntoConstraints = false
        doc.addSubview(stack)
        NSLayoutConstraint.activate([stack.leadingAnchor.constraint(equalTo: doc.leadingAnchor), stack.trailingAnchor.constraint(equalTo: doc.trailingAnchor),
                                     stack.topAnchor.constraint(equalTo: doc.topAnchor), stack.bottomAnchor.constraint(equalTo: doc.bottomAnchor)])

        // hero
        let heroBox = NSView(); heroBox.translatesAutoresizingMaskIntoConstraints = false
        heroBox.wantsLayer = true
        heroView.translatesAutoresizingMaskIntoConstraints = false
        heroBox.addSubview(heroView)
        heroShade.wantsLayer = true
        heroShade.translatesAutoresizingMaskIntoConstraints = false
        let grad = CAGradientLayer()
        grad.colors = [NSColor.clear.cgColor, Theme.panel.cgColor]
        grad.startPoint = CGPoint(x: 0.5, y: 0.3); grad.endPoint = CGPoint(x: 0.5, y: 1)
        heroShade.layer?.addSublayer(grad)
        heroShade.layerContentsRedrawPolicy = .onSetNeedsDisplay
        heroBox.addSubview(heroShade)
        heroBox.addSubview(titleLabel)
        NSLayoutConstraint.activate([
            heroBox.heightAnchor.constraint(equalToConstant: 191),
            heroBox.widthAnchor.constraint(equalToConstant: Theme.detailWidth),
            heroView.leadingAnchor.constraint(equalTo: heroBox.leadingAnchor), heroView.trailingAnchor.constraint(equalTo: heroBox.trailingAnchor),
            heroView.topAnchor.constraint(equalTo: heroBox.topAnchor), heroView.bottomAnchor.constraint(equalTo: heroBox.bottomAnchor),
            heroShade.leadingAnchor.constraint(equalTo: heroBox.leadingAnchor), heroShade.trailingAnchor.constraint(equalTo: heroBox.trailingAnchor),
            heroShade.topAnchor.constraint(equalTo: heroBox.topAnchor), heroShade.bottomAnchor.constraint(equalTo: heroBox.bottomAnchor),
            titleLabel.leadingAnchor.constraint(equalTo: heroBox.leadingAnchor, constant: 16),
            titleLabel.trailingAnchor.constraint(equalTo: heroBox.trailingAnchor, constant: -16),
            titleLabel.bottomAnchor.constraint(equalTo: heroBox.bottomAnchor, constant: -10),
        ])
        heroShadeGradient = grad
        stack.addArrangedSubview(heroBox)

        let body = NSStackView(); body.orientation = .vertical; body.alignment = .leading; body.spacing = 12
        body.edgeInsets = NSEdgeInsets(top: 0, left: 16, bottom: 0, right: 16)
        body.translatesAutoresizingMaskIntoConstraints = false
        stack.addArrangedSubview(body)
        NSLayoutConstraint.activate([body.widthAnchor.constraint(equalToConstant: Theme.detailWidth)])

        body.addArrangedSubview(subtitleLabel)
        pin(subtitleLabel, in: body)

        // actions
        primaryButton.bezelStyle = .rounded; primaryButton.controlSize = .large
        primaryButton.font = Theme.font(14, .semibold)
        primaryButton.keyEquivalent = "\r"
        primaryButton.target = self; primaryButton.action = #selector(primaryTapped)
        primaryButton.translatesAutoresizingMaskIntoConstraints = false
        body.addArrangedSubview(primaryButton)
        pin(primaryButton, in: body)
        body.addArrangedSubview(pendingLabel); pin(pendingLabel, in: body)
        let secondary = NSStackView(views: [storeButton, removeButton]); secondary.spacing = 8; secondary.orientation = .horizontal
        storeButton.bezelStyle = .rounded; removeButton.bezelStyle = .rounded
        storeButton.target = self; storeButton.action = #selector(storeTapped)
        removeButton.target = self; removeButton.action = #selector(removeTapped)
        body.addArrangedSubview(secondary)

        // facts
        metaGrid.translatesAutoresizingMaskIntoConstraints = false
        metaGrid.rowSpacing = 6; metaGrid.columnSpacing = 14
        metaGrid.column(at: 0).xPlacement = .trailing
        body.addArrangedSubview(metaGrid); pin(metaGrid, in: body)

        // compatibility
        body.addArrangedSubview(sectionTitle("Compatibility"))
        let compatRow = NSStackView(views: [compatPill, compatLabel]); compatRow.spacing = 8
        compatPill.translatesAutoresizingMaskIntoConstraints = false
        body.addArrangedSubview(compatRow)
        body.addArrangedSubview(compatReasons); pin(compatReasons, in: body)
        compatButton.bezelStyle = .rounded; compatButton.controlSize = .small
        compatButton.target = self; compatButton.action = #selector(compatTapped)
        body.addArrangedSubview(compatButton)

        // settings
        body.addArrangedSubview(sectionTitle("Launch settings for this game"))
        settingsBox = NSStackView(); settingsBox.orientation = .vertical; settingsBox.alignment = .leading; settingsBox.spacing = 6
        settingsBox.translatesAutoresizingMaskIntoConstraints = false
        for (label, popup, items) in [("Renderer", rendererPopup, ["Default", "dxmt", "d3dmetal", "wined3d"]),
                                      ("FPS cap", fpsPopup, ["Default", "Off", "30", "40", "60", "120"]),
                                      ("Retina", retinaPopup, ["Default", "On", "Off"]),
                                      ("MetalFX", metalfxPopup, ["Default", "Off", "1.5x", "2x"])] {
            popup.addItems(withTitles: items)
            popup.controlSize = .small; popup.font = Theme.font(11)
            popup.target = self; popup.action = #selector(settingChanged(_:))
            popup.translatesAutoresizingMaskIntoConstraints = false
            let l = makeLabel(label, size: 12, color: Theme.textSecondary)
            l.translatesAutoresizingMaskIntoConstraints = false
            let row = NSStackView(views: [l, popup]); row.spacing = 8
            row.translatesAutoresizingMaskIntoConstraints = false
            NSLayoutConstraint.activate([l.widthAnchor.constraint(equalToConstant: 70), popup.widthAnchor.constraint(equalToConstant: 140)])
            settingsBox.addArrangedSubview(row)
        }
        body.addArrangedSubview(settingsBox)
        let footRow = NSStackView(views: [footnote, savedLabel]); footRow.spacing = 8
        body.addArrangedSubview(footRow); pin(footRow, in: body)
        profileButton.bezelStyle = .rounded; profileButton.controlSize = .small
        profileButton.target = self; profileButton.action = #selector(profileTapped)
        body.addArrangedSubview(profileButton)
        body.addArrangedSubview(notesLabel); pin(notesLabel, in: body)

        emptyLabel.translatesAutoresizingMaskIntoConstraints = false
        addSubview(emptyLabel)
        NSLayoutConstraint.activate([emptyLabel.centerXAnchor.constraint(equalTo: centerXAnchor), emptyLabel.centerYAnchor.constraint(equalTo: centerYAnchor)])
        show(nil)
    }
    required init?(coder: NSCoder) { fatalError() }
    private var heroShadeGradient: CAGradientLayer?

    override func layout() {
        super.layout()
        heroShadeGradient?.frame = heroShade.bounds
    }

    private func pin(_ v: NSView, in s: NSStackView) {
        NSLayoutConstraint.activate([v.leadingAnchor.constraint(equalTo: s.leadingAnchor, constant: s.edgeInsets.left),
                                     v.trailingAnchor.constraint(equalTo: s.trailingAnchor, constant: -s.edgeInsets.right)])
    }
    private func sectionTitle(_ t: String) -> NSTextField {
        let l = makeLabel(t.uppercased(), size: 10, weight: .semibold, color: Theme.textTertiary)
        return l
    }

    private func metaRow(_ k: String, _ v: String) {
        let kl = makeLabel(k, size: 11, color: Theme.textTertiary)
        let vl = makeLabel(v, size: 12, lines: 2)
        metaGrid.addRow(with: [kl, vl])
    }

    func show(_ g: Game?) {
        game = g
        scroll.isHidden = g == nil
        emptyLabel.isHidden = g != nil
        guard let g = g else { return }
        titleLabel.stringValue = g.name
        var sub: [String] = []
        if let d = g.developer, !d.isEmpty { sub.append(d) }
        if g.isMac { sub.append("native Mac Steam") }
        subtitleLabel.stringValue = sub.joined(separator: " · ")
        subtitleLabel.isHidden = sub.isEmpty

        while metaGrid.numberOfRows > 0 { metaGrid.removeRow(at: 0) }
        let pending = Backend.shared.pending[g.appid]
        var status: String
        if let p = pending { status = p.message }
        else if g.downloading { status = String(format: "Downloading %.0f%%", g.progress * 100) + ((g.paused ?? false) ? " (paused)" : "") }
        else if g.isRunning { status = "Running" }
        else if g.installed && g.needs_update { status = "Installed, update needed" }
        else if g.installed { status = g.ready ? "Installed" : "Installed (" + (g.state?.joined(separator: ", ") ?? "not ready") + ")" }
        else { status = g.owned ? "Not installed" : "Free title, not installed" }
        metaRow("Status", status)
        if g.installed { metaRow("Size on disk", Format.size(g.size)) }
        else if let t = g.bytes_to_download, t > 0 { metaRow("Download", Format.size(t)) }
        metaRow("Last played", Format.lastPlayed(g.last_played) + (Format.playtime(g.playtime_min).isEmpty ? "" : " · " + Format.playtime(g.playtime_min) + " total"))
        if !g.isMac {
            var r = g.renderer
            if let d = g.dx12 { r += d ? " (DirectX 12)" : " (DirectX 11)" }
            metaRow("Renderer", r)
        }
        if let deck = g.deck_category, deck > 0 {
            metaRow("Steam Deck", ["", "Unsupported", "Playable", "Verified"][min(deck, 3)])
        }
        if let cs = g.controller_support, !cs.isEmpty { metaRow("Controller", cs.capitalizedFirst) }
        if let dlc = g.dlc_owned, !dlc.isEmpty { metaRow("DLC owned", "\(dlc.count)") }
        metaRow("AppID", g.appid)

        compatPill.kind = .pending
        compatLabel.stringValue = g.compat.label
        compatLabel.textColor = Theme.verdictColor(g.compat.verdict)
        compatPill.text = Theme.verdictGlyph(g.compat.verdict)
        let reasons = g.compat.reasons ?? []
        compatReasons.stringValue = reasons.isEmpty ? (g.compat.verdict == "unknown" ? "Not checked yet." : "") : reasons.map { "• " + $0 }.joined(separator: "\n")
        compatReasons.isHidden = compatReasons.stringValue.isEmpty
        compatButton.title = g.compat.verdict == "unknown" ? "Check compatibility" : "Re-check"
        compatButton.isHidden = g.isMac

        switch g.primary {
        case .play: primaryButton.title = "Play"; primaryButton.isEnabled = pending == nil
        case .install: primaryButton.title = g.owned ? "Install" : "Install (free)"; primaryButton.isEnabled = pending == nil
        case .update: primaryButton.title = "Update"; primaryButton.isEnabled = pending == nil
        case .downloading: primaryButton.title = (g.paused ?? false) ? "Paused in Steam" : "Downloading…"; primaryButton.isEnabled = false
        case .running: primaryButton.title = "Running"; primaryButton.isEnabled = false
        }
        if let f = Backend.shared.failed[g.appid] { pendingLabel.stringValue = f; pendingLabel.textColor = Theme.red; pendingLabel.isHidden = false }
        else if let p = pending { pendingLabel.stringValue = p.message; pendingLabel.textColor = Theme.blue; pendingLabel.isHidden = false }
        else if Backend.shared.library?.steam.launch_prompt_appid == g.appid {
            pendingLabel.stringValue = "Steam wants an answer before it starts the game: look at its window."; pendingLabel.textColor = Theme.amber; pendingLabel.isHidden = false
        } else { pendingLabel.isHidden = true }
        removeButton.isHidden = !g.installed || g.isMac || g.downloading
        removeButton.isEnabled = pending == nil && !g.isRunning

        settingsBox.isHidden = g.isMac
        let s = g.settings ?? [:]
        select(rendererPopup, s["renderer"]?.description, map: ["dxmt": 1, "d3dmetal": 2, "wined3d": 3])
        if let f = s["fps_cap"]?.description { select(fpsPopup, f == "0" ? "Off" : f, map: ["Off": 1, "30": 2, "40": 3, "60": 4, "120": 5]) } else { fpsPopup.selectItem(at: 0) }
        if let r = s["retina"]?.description { retinaPopup.selectItem(at: r == "on" ? 1 : 2) } else { retinaPopup.selectItem(at: 0) }
        if let m = s["metalfx"]?.description { select(metalfxPopup, m, map: ["1.5": 2, "2": 3]) } else { metalfxPopup.selectItem(at: 0) }
        var foot = "Applies at the next launch; Steam restarts if needed."
        if let e = g.effective { foot += " In effect: \(e.renderer ?? "auto"), fps cap \(e.fps_cap.map { $0 == 0 ? "off" : String($0) } ?? "off"), retina \((e.retina ?? false) ? "on" : "off")" + ((e.metalfx ?? 0) > 1 ? String(format: ", MetalFX %gx", e.metalfx!) : "") + "." }
        footnote.stringValue = foot
        let prof = g.profile
        profileButton.isHidden = !(prof?.available ?? false) || g.isMac
        profileButton.title = (prof?.applied ?? false) ? "Re-apply recommended settings" : "Apply recommended settings"
        let notes = prof?.notes ?? []
        notesLabel.stringValue = notes.isEmpty ? "" : "Notes\n" + notes.map { "• " + $0 }.joined(separator: "\n")
        notesLabel.isHidden = notes.isEmpty

        let hero = CoverStore.shared.image(for: g, kind: .hero) { [weak self] img in
            guard let self = self, self.game?.appid == g.appid, let img = img else { return }
            self.heroView.image = img
        }
        heroView.image = hero ?? CoverStore.shared.image(for: g, kind: .header) { [weak self] img in
            guard let self = self, self.game?.appid == g.appid, let img = img, self.heroView.image == nil else { return }
            self.heroView.image = img
        } ?? CoverStore.placeholder(name: "", appid: g.appid, size: NSSize(width: 680, height: 382))
        needsLayout = true
    }

    private func select(_ popup: NSPopUpButton, _ value: String?, map: [String: Int]) {
        guard let v = value, let i = map[v] ?? map[v.replacingOccurrences(of: ".0", with: "")] else { popup.selectItem(at: 0); return }
        popup.selectItem(at: i)
    }

    @objc private func primaryTapped() { if let g = game { onPrimary?(g) } }
    @objc private func removeTapped() { if let g = game { onRemove?(g) } }
    @objc private func storeTapped() {
        guard let g = game else { return }
        if Backend.shared.library?.steam.running == true { Backend.shared.run(["store", g.appid]) { _ in } }
        else if let u = URL(string: "https://store.steampowered.com/app/\(g.appid)") { NSWorkspace.shared.open(u) }
    }
    @objc private func compatTapped() {
        guard let g = game else { return }
        compatButton.isEnabled = false; compatButton.title = "Checking…"
        Backend.shared.checkCompat(g.appid) { [weak self] _ in self?.compatButton.isEnabled = true }
    }
    @objc private func profileTapped() {
        guard let g = game else { return }
        profileButton.isEnabled = false
        Backend.shared.resetProfile(g.appid) { [weak self] _ in self?.profileButton.isEnabled = true; self?.flashSaved() }
    }
    @objc private func settingChanged(_ sender: NSPopUpButton) {
        guard let g = game else { return }
        let key: String, value: String
        let idx = sender.indexOfSelectedItem
        switch sender {
        case rendererPopup: key = "renderer"; value = idx == 0 ? "default" : sender.titleOfSelectedItem ?? "default"
        case fpsPopup: key = "fps_cap"; value = idx == 0 ? "default" : (idx == 1 ? "0" : sender.titleOfSelectedItem ?? "default")
        case retinaPopup: key = "retina"; value = idx == 0 ? "default" : (idx == 1 ? "on" : "off")
        default: key = "metalfx"; value = idx == 0 ? "default" : (idx == 1 ? "0" : (idx == 2 ? "1.5" : "2"))
        }
        Backend.shared.setSetting(g.appid, key: key, value: value) { [weak self] r in if r.status == 0 { self?.flashSaved() } }
    }
    private func flashSaved() {
        savedLabel.stringValue = "✓ saved"
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in self?.savedLabel.stringValue = "" }
    }
}

final class FlippedView: NSView { override var isFlipped: Bool { true } }

/// Draws its image scaled to fill the bounds, cropping the long side (hero art is 3:1, the box is 16:9).
final class AspectFillImageView: NSView {
    var image: NSImage? { didSet { needsDisplay = true } }
    override func draw(_ dirtyRect: NSRect) {
        Theme.panelRaised.setFill(); bounds.fill()
        guard let img = image, img.size.width > 0, img.size.height > 0 else { return }
        let scale = max(bounds.width / img.size.width, bounds.height / img.size.height)
        let size = NSSize(width: img.size.width * scale, height: img.size.height * scale)
        let origin = NSPoint(x: (bounds.width - size.width) / 2, y: (bounds.height - size.height) / 2)
        img.draw(in: NSRect(origin: origin, size: size), from: .zero, operation: .sourceOver, fraction: 1, respectFlipped: true, hints: [.interpolation: NSImageInterpolation.high])
    }
}
