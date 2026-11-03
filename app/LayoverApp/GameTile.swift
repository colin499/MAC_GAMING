import AppKit

protocol GameTileDelegate: AnyObject {
    func tilePrimaryAction(_ game: Game)
    func tileHovered(_ game: Game?)
}

/// A small text pill (Installed, Downloading 37%, Update needed, ...).
final class BadgeView: NSView {
    var text = "" { didSet { invalidateIntrinsicContentSize(); needsDisplay = true } }
    var kind: BadgeKind = .notInstalled { didSet { needsDisplay = true } }
    private let font = Theme.font(10, .semibold)
    override var intrinsicContentSize: NSSize {
        let w = (text as NSString).size(withAttributes: [.font: font]).width
        return NSSize(width: ceil(w) + 16, height: 18)
    }
    override func draw(_ dirtyRect: NSRect) {
        let (fill, color) = Theme.badgeColors(kind)
        let path = NSBezierPath(roundedRect: bounds, xRadius: 9, yRadius: 9)
        NSColor.black.withAlphaComponent(0.45).setFill(); path.fill()
        fill.setFill(); path.fill()
        let attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: color]
        let size = (text as NSString).size(withAttributes: attrs)
        (text as NSString).draw(at: NSPoint(x: (bounds.width - size.width) / 2, y: (bounds.height - size.height) / 2), withAttributes: attrs)
    }
}

/// Thin progress bar; `fraction` nil = indeterminate (a static segment, so snapshots render it).
final class ProgressBarView: NSView {
    var fraction: Double? = 0 { didSet { needsDisplay = true } }
    var color = Theme.blue
    override func draw(_ dirtyRect: NSRect) {
        NSColor.black.withAlphaComponent(0.55).setFill()
        NSBezierPath(roundedRect: bounds, xRadius: 2, yRadius: 2).fill()
        color.setFill()
        if let f = fraction {
            let w = max(3, bounds.width * CGFloat(min(max(f, 0), 1)))
            NSBezierPath(roundedRect: NSRect(x: 0, y: 0, width: w, height: bounds.height), xRadius: 2, yRadius: 2).fill()
        } else {
            let w = bounds.width * 0.3
            let x = (bounds.width - w) * CGFloat((Date().timeIntervalSince1970 / 1.2).truncatingRemainder(dividingBy: 1))
            NSBezierPath(roundedRect: NSRect(x: x, y: 0, width: w, height: bounds.height), xRadius: 2, yRadius: 2).fill()
        }
    }
}

final class GameTileItem: NSCollectionViewItem {
    static let identifier = NSUserInterfaceItemIdentifier("GameTile")
    var tile: GameTileView { view as! GameTileView }
    override func loadView() { view = GameTileView(frame: NSRect(x: 0, y: 0, width: Theme.tileWidth, height: Theme.tileHeight)) }
    override var isSelected: Bool { didSet { tile.selected = isSelected } }
}

final class GameTileView: NSView {
    private let coverView = NSImageView()
    private let nameLabel = makeLabel("", size: 12, weight: .medium, lines: 2)
    private let statusLabel = makeLabel("", size: 11, color: Theme.textSecondary)
    private let badge = BadgeView()
    private let compatGlyph = makeLabel("", size: 11, weight: .bold)
    private let progress = ProgressBarView()
    private let overlay = NSView()
    private let button = NSButton(title: "", target: nil, action: nil)
    private var tracking: NSTrackingArea?
    private(set) var game: Game?
    weak var delegate: GameTileDelegate?
    var selected = false { didSet { needsDisplay = true } }
    private var hovered = false { didSet { overlay.isHidden = !hovered || !canAct; needsDisplay = true } }
    private var canAct = false

    override var isFlipped: Bool { true }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        coverView.wantsLayer = true
        coverView.layer?.cornerRadius = Theme.tileCorner
        coverView.layer?.masksToBounds = true
        coverView.layer?.borderWidth = 1
        coverView.layer?.borderColor = NSColor.white.withAlphaComponent(0.10).cgColor
        coverView.imageScaling = .scaleAxesIndependently
        coverView.frame = NSRect(x: 0, y: 0, width: Theme.tileWidth, height: Theme.tileCoverHeight)
        addSubview(coverView)

        overlay.wantsLayer = true
        overlay.frame = coverView.frame
        let grad = CAGradientLayer()
        grad.colors = [NSColor.clear.cgColor, NSColor.black.withAlphaComponent(0.75).cgColor]
        grad.startPoint = CGPoint(x: 0.5, y: 0.35); grad.endPoint = CGPoint(x: 0.5, y: 1)
        grad.cornerRadius = Theme.tileCorner
        grad.frame = overlay.bounds
        overlay.layer?.addSublayer(grad)
        overlay.isHidden = true
        button.bezelStyle = .rounded
        button.controlSize = .regular
        button.font = Theme.font(13, .semibold)
        button.target = self; button.action = #selector(primary)
        button.frame = NSRect(x: 25, y: Theme.tileCoverHeight - 52, width: 100, height: 32)
        overlay.addSubview(button)
        addSubview(overlay)

        badge.frame = NSRect(x: 6, y: 6, width: 10, height: 18)
        addSubview(badge)
        compatGlyph.frame = NSRect(x: Theme.tileWidth - 26, y: 5, width: 20, height: 20)
        compatGlyph.alignment = .center
        compatGlyph.wantsLayer = true
        compatGlyph.layer?.cornerRadius = 10
        compatGlyph.layer?.backgroundColor = NSColor.black.withAlphaComponent(0.6).cgColor
        addSubview(compatGlyph)
        progress.frame = NSRect(x: 0, y: Theme.tileCoverHeight - 4, width: Theme.tileWidth, height: 4)
        progress.isHidden = true
        addSubview(progress)

        nameLabel.frame = NSRect(x: 2, y: Theme.tileCoverHeight + 6, width: Theme.tileWidth - 4, height: 34)
        nameLabel.translatesAutoresizingMaskIntoConstraints = true
        statusLabel.frame = NSRect(x: 2, y: Theme.tileCoverHeight + 42, width: Theme.tileWidth - 4, height: 16)
        statusLabel.translatesAutoresizingMaskIntoConstraints = true
        addSubview(nameLabel); addSubview(statusLabel)
        setAccessibilityElement(true)
        setAccessibilityRole(.button)
    }
    required init?(coder: NSCoder) { fatalError() }

    func configure(_ game: Game, pending: PendingAction?, failure: String?, delegate: GameTileDelegate?) {
        self.game = game
        self.delegate = delegate
        nameLabel.stringValue = game.name
        let status: String
        if let p = pending {
            badge.kind = .pending; badge.text = p.kind == .remove ? "Removing…" : (p.kind == .play ? "Launching…" : "Waiting for Steam…")
            progress.isHidden = false; progress.fraction = nil; progress.color = Theme.blue
            status = p.message
        } else if game.downloading {
            let pct = Int((game.progress * 100).rounded())
            badge.kind = .downloading; badge.text = (game.paused ?? false) ? "Paused \(pct)%" : "Downloading \(pct)%"
            progress.isHidden = false; progress.fraction = game.progress; progress.color = (game.paused ?? false) ? Theme.amber : Theme.blue
            if let t = game.bytes_to_download, t > 0 { status = "\(Format.size(game.bytes_downloaded ?? 0)) of \(Format.size(t))" }
            else { status = game.state?.contains("preallocating") == true ? "Preparing disk space…" : "Starting download…" }
        } else if game.isRunning {
            badge.kind = .running; badge.text = "Running"; progress.isHidden = true
            status = "Running now"
        } else if game.isMac {
            badge.kind = .mac; badge.text = "Mac"; progress.isHidden = true
            status = Format.size(game.size)
        } else if game.installed && game.needs_update {
            badge.kind = .update; badge.text = "Update needed"; progress.isHidden = true
            status = Format.size(game.size)
        } else if game.installed {
            badge.kind = .installed; badge.text = "Installed"; progress.isHidden = true
            status = Format.size(game.size) + (game.last_played ?? 0 > 0 ? " · " + Format.lastPlayed(game.last_played) : "")
        } else {
            badge.kind = .notInstalled; badge.text = game.owned ? "Not installed" : "Free"; progress.isHidden = true
            status = game.owned ? "Owned" : "Free to install"
        }
        if let f = failure {
            badge.kind = .failed; badge.text = "Failed"
            statusLabel.stringValue = f; statusLabel.textColor = Theme.red
        } else {
            statusLabel.stringValue = status; statusLabel.textColor = Theme.textSecondary
        }
        badge.frame.size = badge.intrinsicContentSize
        compatGlyph.stringValue = Theme.verdictGlyph(game.compat.verdict)
        compatGlyph.textColor = Theme.verdictColor(game.compat.verdict)
        compatGlyph.isHidden = game.compat.verdict == "unknown"
        compatGlyph.toolTip = game.compat.label + (game.compat.reasons?.isEmpty == false ? ": " + game.compat.reasons!.joined(separator: "; ") : "")
        switch game.primary {
        case .play: button.title = "Play"; canAct = pending == nil
        case .install: button.title = "Install"; canAct = pending == nil
        case .update: button.title = "Update"; canAct = pending == nil
        case .downloading, .running: button.title = ""; canAct = false
        }
        button.isEnabled = canAct
        overlay.isHidden = !hovered || !canAct
        setAccessibilityLabel("\(game.name), \(badge.text)")
        toolTip = game.name
        let img = CoverStore.shared.image(for: game, kind: .capsule) { [weak self] loaded in
            guard let self = self, self.game?.appid == game.appid else { return }
            if let l = loaded { self.coverView.image = l }
        }
        coverView.image = img ?? CoverStore.placeholder(name: game.name, appid: game.appid, size: NSSize(width: 300, height: 450))
    }

    @objc private func primary() {
        guard let g = game, canAct else { return }
        delegate?.tilePrimaryAction(g)
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let t = tracking { removeTrackingArea(t) }
        let t = NSTrackingArea(rect: coverView.frame, options: [.mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect], owner: self, userInfo: nil)
        addTrackingArea(t); tracking = t
    }
    override func mouseEntered(with event: NSEvent) { hovered = true; delegate?.tileHovered(game) }
    override func mouseExited(with event: NSEvent) { hovered = false; delegate?.tileHovered(nil) }
    override func mouseDown(with event: NSEvent) {
        if event.clickCount == 2, let g = game, canAct { delegate?.tilePrimaryAction(g); return }
        super.mouseDown(with: event)
    }

    override func draw(_ dirtyRect: NSRect) {
        guard selected else { return }
        let r = coverView.frame.insetBy(dx: -3, dy: -3)
        let p = NSBezierPath(roundedRect: r, xRadius: Theme.tileCorner + 3, yRadius: Theme.tileCorner + 3)
        p.lineWidth = 2
        Theme.accent.setStroke(); p.stroke()
    }
}
