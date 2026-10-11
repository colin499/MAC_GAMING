// LayoverMenu: Layover.app's game picker, styled like a pinball backglass (dot-matrix display, insert lights,
// flippers). Build: swiftc -O -swift-version 5 -framework AppKit -o LayoverMenu app/menu.swift
// Usage: LayoverMenu [--game NAME]... [--action NAME]... [--status "DMD LINE"] [--snapshot out.png]
// Prints the chosen name on stdout and exits 0; Esc / close = exit 1. --snapshot renders the window to a PNG and exits.
import AppKit

var games: [String] = [], actions: [String] = [], status = "READY PLAYER 1", snapshot: String? = nil
var it = CommandLine.arguments.dropFirst().makeIterator()
while let a = it.next() {
    switch a {
    case "--game": if let v = it.next() { games.append(v) }
    case "--action": if let v = it.next() { actions.append(v) }
    case "--status": if let v = it.next() { status = v }
    case "--snapshot": if let v = it.next() { snapshot = v }
    default: break
    }
}
if games.isEmpty && actions.isEmpty { actions = ["Open Steam", "Quit Steam"] }

// ---------------------------------------------------------------- palette
func rgb(_ r: Int, _ g: Int, _ b: Int, _ a: CGFloat = 1) -> NSColor { NSColor(calibratedRed: CGFloat(r) / 255, green: CGFloat(g) / 255, blue: CGFloat(b) / 255, alpha: a) }
let bgTop = rgb(14, 10, 34), bgBottom = rgb(3, 2, 10)
let dmdLit = rgb(255, 140, 20), dmdDim = rgb(52, 18, 6), dmdGlass = rgb(10, 4, 2)
let insertLit = rgb(255, 196, 40), insertLitEdge = rgb(255, 110, 20), insertDim = rgb(70, 40, 20), insertDimEdge = rgb(40, 22, 12)
let rolloverLit = rgb(90, 240, 255), rolloverDim = rgb(24, 60, 80)
let flipperRed = rgb(220, 40, 50), chrome = rgb(200, 205, 220)

func heavyFont(_ size: CGFloat) -> NSFont {
    for n in ["Futura-CondensedExtraBold", "AvenirNext-Heavy", "HelveticaNeue-CondensedBlack", "Impact"] { if let f = NSFont(name: n, size: size) { return f } }
    return NSFont.boldSystemFont(ofSize: size)
}
func dmdFont(_ size: CGFloat) -> NSFont {
    for n in ["Menlo-Bold", "Monaco", "Courier-Bold"] { if let f = NSFont(name: n, size: size) { return f } }
    return NSFont.monospacedSystemFont(ofSize: size, weight: .bold)
}

// ---------------------------------------------------------------- DMD: a 128x32 dot grid rendered from text
final class DMDView: NSView {
    var lines: [(String, CGFloat)] = []            // (text, font size in dot units)
    let cols = 128, rows = 32
    var pitch: CGFloat { bounds.width / CGFloat(cols) }
    var scroll: CGFloat = 0                        // marquee offset in dots for lines wider than the display
    override var isFlipped: Bool { true }
    func bitmap() -> NSBitmapImageRep {
        let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: cols, pixelsHigh: rows, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
        NSGraphicsContext.saveGraphicsState()
        let ctx = NSGraphicsContext(bitmapImageRep: rep)!; NSGraphicsContext.current = ctx
        ctx.shouldAntialias = false
        var y: CGFloat = 1   // the bitmap's y axis points up: draw the last line first, from the bottom
        for (text, size) in lines.reversed() {
            let f = heavyFont(size)
            let p = NSMutableParagraphStyle(); p.alignment = .center; p.lineBreakMode = .byClipping
            let s = NSAttributedString(string: text, attributes: [.font: f, .foregroundColor: NSColor.white, .paragraphStyle: p, .kern: 1])
            let sz = s.size()
            if sz.width > CGFloat(cols) {   // marquee: slide through, wrap with a gap
                let span = sz.width + 24
                let x = CGFloat(cols) - scroll.truncatingRemainder(dividingBy: span)
                s.draw(at: NSPoint(x: x, y: y)); s.draw(at: NSPoint(x: x - span, y: y))
            } else {
                s.draw(in: NSRect(x: 0, y: y, width: CGFloat(cols), height: sz.height))
            }
            y += sz.height - 1
        }
        NSGraphicsContext.restoreGraphicsState()
        return rep
    }
    override func draw(_ dirtyRect: NSRect) {
        dmdGlass.setFill(); NSBezierPath(roundedRect: bounds, xRadius: 6, yRadius: 6).fill()
        let rep = bitmap(); let p = pitch; let r = p * 0.36
        for row in 0..<rows {
            for col in 0..<cols {
                let c = rep.colorAt(x: col, y: row)
                let lit = (c?.alphaComponent ?? 0) > 0.45
                (lit ? dmdLit : dmdDim).setFill()
                let cx = CGFloat(col) * p + p / 2, cy = CGFloat(row) * p + p / 2
                NSBezierPath(ovalIn: NSRect(x: cx - r, y: cy - r, width: 2 * r, height: 2 * r)).fill()
                if lit {   // soft glow
                    dmdLit.withAlphaComponent(0.22).setFill()
                    NSBezierPath(ovalIn: NSRect(x: cx - r * 1.9, y: cy - r * 1.9, width: 3.8 * r, height: 3.8 * r)).fill()
                }
            }
        }
    }
}

// ---------------------------------------------------------------- the backglass
final class Backglass: NSView {
    struct Item { let name: String; let isGame: Bool; var rect: NSRect = .zero }
    var items: [Item] = []
    var selected = 0
    var onChoose: ((String) -> Void)?
    var onCancel: (() -> Void)?
    var pulse: CGFloat = 1
    var pulseDir: CGFloat = -1
    var stars: [(CGFloat, CGFloat, CGFloat)] = []
    let dmd = DMDView()
    var tracking: NSTrackingArea?

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }

    init(games: [String], actions: [String], status: String, frame: NSRect) {
        super.init(frame: frame)
        items = games.map { Item(name: $0, isGame: true) } + actions.map { Item(name: $0, isGame: false) }
        var seed: UInt32 = 20261011
        func rnd() -> CGFloat { seed = seed &* 1664525 &+ 1013904223; return CGFloat(seed >> 8) / CGFloat(1 << 24) }
        for _ in 0..<70 { stars.append((rnd(), rnd(), 0.4 + rnd() * 1.2)) }
        dmd.lines = [("LAYOVER", 15), (status.uppercased(), 8)]
        addSubview(dmd)
        layoutItems()
        Timer.scheduledTimer(withTimeInterval: 1.0 / 24, repeats: true) { [weak self] _ in
            guard let s = self else { return }
            s.pulse += 0.045 * s.pulseDir
            if s.pulse < 0.55 { s.pulseDir = 1 } else if s.pulse > 1 { s.pulseDir = -1 }
            s.setNeedsDisplay(s.itemsRegion)
        }
        Timer.scheduledTimer(withTimeInterval: 1.0 / 12, repeats: true) { [weak self] _ in
            guard let s = self else { return }
            s.dmd.scroll += 1; s.dmd.needsDisplay = true
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    var itemsRegion: NSRect { items.map { $0.rect }.reduce(NSRect.null) { $0.union($1) }.insetBy(dx: -30, dy: -30) }

    static func height(games: Int, actions: Int) -> CGFloat {
        let rows = CGFloat((actions + 1) / 2)
        return 40 + 136 + 26 + CGFloat(games) * 70 + 18 + rows * 56 + 90
    }

    func layoutItems() {
        let w = bounds.width
        dmd.frame = NSRect(x: 24, y: 40, width: w - 48, height: (w - 48) / 4)
        var y = dmd.frame.maxY + 26
        for i in items.indices where items[i].isGame {
            items[i].rect = NSRect(x: 64, y: y, width: w - 128, height: 56); y += 70
        }
        y += 18
        var col = 0
        let bw = (w - 128 - 16) / 2
        for i in items.indices where !items[i].isGame {
            items[i].rect = NSRect(x: 64 + CGFloat(col) * (bw + 16), y: y, width: bw, height: 42)
            col += 1
            if col == 2 { col = 0; y += 56 }
        }
    }

    override func updateTrackingAreas() {
        if let t = tracking { removeTrackingArea(t) }
        tracking = NSTrackingArea(rect: bounds, options: [.mouseMoved, .activeAlways, .inVisibleRect], owner: self, userInfo: nil)
        addTrackingArea(tracking!)
        super.updateTrackingAreas()
    }
    override func mouseMoved(with e: NSEvent) {
        let p = convert(e.locationInWindow, from: nil)
        if let i = items.firstIndex(where: { $0.rect.contains(p) }), i != selected { selected = i; needsDisplay = true }
    }
    override func mouseDown(with e: NSEvent) {
        let p = convert(e.locationInWindow, from: nil)
        if let i = items.firstIndex(where: { $0.rect.contains(p) }) { selected = i; onChoose?(items[i].name) }
    }
    override func keyDown(with e: NSEvent) {
        switch e.keyCode {
        case 53: onCancel?()                                   // esc
        case 36, 76, 49: onChoose?(items[selected].name)       // return, enter, space
        case 126, 123: selected = (selected - 1 + items.count) % items.count; needsDisplay = true   // up, left
        case 125, 124, 48: selected = (selected + 1) % items.count; needsDisplay = true              // down, right, tab
        default:
            if let ch = e.characters, let n = Int(ch), n >= 1, n <= items.count { selected = n - 1; onChoose?(items[n - 1].name) }
        }
    }

    func label(_ text: String, in rect: NSRect, font: NSFont, color: NSColor, align: NSTextAlignment = .center, shadow: Bool = true) {
        let p = NSMutableParagraphStyle(); p.alignment = align; p.lineBreakMode = .byTruncatingTail
        var attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: color, .paragraphStyle: p, .kern: 1.2]
        if shadow { let sh = NSShadow(); sh.shadowColor = NSColor.black.withAlphaComponent(0.8); sh.shadowOffset = NSSize(width: 0, height: -1.5); sh.shadowBlurRadius = 2; attrs[.shadow] = sh }
        let s = NSAttributedString(string: text, attributes: attrs)
        let h = s.size().height
        s.draw(in: NSRect(x: rect.minX, y: rect.midY - h / 2, width: rect.width, height: h))
    }

    override func draw(_ dirtyRect: NSRect) {
        let w = bounds.width, h = bounds.height
        NSGradient(starting: bgTop, ending: bgBottom)!.draw(in: bounds, angle: 90)
        for (sx, sy, r) in stars {   // starfield
            NSColor.white.withAlphaComponent(0.35 + 0.5 * r / 1.6).setFill()
            NSBezierPath(ovalIn: NSRect(x: sx * w, y: sy * h, width: r, height: r)).fill()
        }
        // cabinet frame
        chrome.withAlphaComponent(0.55).setStroke()
        let frame = NSBezierPath(roundedRect: bounds.insetBy(dx: 10, dy: 10), xRadius: 18, yRadius: 18); frame.lineWidth = 3; frame.stroke()
        rgb(120, 20, 40, 0.9).setStroke()
        let frame2 = NSBezierPath(roundedRect: bounds.insetBy(dx: 16, dy: 16), xRadius: 14, yRadius: 14); frame2.lineWidth = 1.5; frame2.stroke()
        // header plate above the DMD
        label("WHAT DO YOU WANT TO PLAY?", in: NSRect(x: 0, y: 14, width: w, height: 24), font: heavyFont(13), color: rgb(255, 220, 120, 0.9))
        // pop bumpers beside game inserts; rollover rectangles for actions
        for (i, item) in items.enumerated() {
            let sel = i == selected
            if item.isGame {
                let r = item.rect
                let glow = sel ? pulse : 0
                if sel {   // halo
                    let halo = NSBezierPath(roundedRect: r.insetBy(dx: -8, dy: -8), xRadius: 36, yRadius: 36)
                    insertLit.withAlphaComponent(0.18 * glow + 0.08).setFill(); halo.fill()
                }
                let path = NSBezierPath(roundedRect: r, xRadius: r.height / 2, yRadius: r.height / 2)
                let a = sel ? insertLit.blended(withFraction: 1 - glow, of: insertLitEdge)! : insertDim
                let b = sel ? insertLitEdge : insertDimEdge
                NSGradient(starting: a, ending: b)!.draw(in: path, angle: 90)
                (sel ? rgb(255, 240, 200) : chrome.withAlphaComponent(0.5)).setStroke(); path.lineWidth = sel ? 2.5 : 1.5; path.stroke()
                // ball number bumper
                let br = NSRect(x: r.minX - 46, y: r.midY - 16, width: 32, height: 32)
                NSGradient(starting: sel ? rgb(255, 90, 90) : rgb(120, 40, 50), ending: sel ? rgb(160, 20, 30) : rgb(60, 15, 25))!.draw(in: NSBezierPath(ovalIn: br), angle: 90)
                chrome.setStroke(); let bp = NSBezierPath(ovalIn: br); bp.lineWidth = 2; bp.stroke()
                label("\(i + 1)", in: br, font: heavyFont(16), color: .white)
                label(item.name.uppercased(), in: r.insetBy(dx: 18, dy: 0), font: heavyFont(22), color: sel ? rgb(40, 16, 0) : rgb(255, 215, 150), shadow: !sel)
                if sel { label("▶", in: NSRect(x: r.maxX + 14, y: r.minY, width: 30, height: r.height), font: heavyFont(18), color: insertLit.withAlphaComponent(0.5 + 0.5 * glow)) }
            } else {
                let r = item.rect
                let path = NSBezierPath(roundedRect: r, xRadius: 8, yRadius: 8)
                (sel ? rolloverLit.withAlphaComponent(0.35 + 0.35 * pulse) : rgb(10, 20, 30, 0.9)).setFill(); path.fill()
                (sel ? rolloverLit : rolloverDim).setStroke(); path.lineWidth = sel ? 2.5 : 1.5; path.stroke()
                // rollover lane stripes
                rolloverDim.withAlphaComponent(0.5).setFill()
                for k in 0..<3 { NSBezierPath(rect: NSRect(x: r.minX + 10 + CGFloat(k) * 6, y: r.minY + 8, width: 2, height: r.height - 16)).fill() }
                label(item.name.uppercased(), in: r.insetBy(dx: 28, dy: 0), font: heavyFont(13), color: sel ? .white : rolloverLit.withAlphaComponent(0.85))
            }
        }
        // flippers + plunger hint
        let fy = h - 58
        for (x, dir) in [(w * 0.30, 1.0), (w * 0.70, -1.0)] {
            let t = NSAffineTransform(); t.translateX(by: x, yBy: fy); t.rotate(byDegrees: CGFloat(-22 * dir))
            let fl = NSBezierPath(roundedRect: NSRect(x: dir > 0 ? 0 : -78, y: -8, width: 78, height: 16), xRadius: 8, yRadius: 8)
            fl.transform(using: t as AffineTransform)
            NSGradient(starting: flipperRed, ending: rgb(120, 10, 20))!.draw(in: fl, angle: 90); chrome.setStroke(); fl.lineWidth = 1.5; fl.stroke()
        }
        // ball
        let ball = NSRect(x: w / 2 - 9, y: fy - 30, width: 18, height: 18)
        NSGradient(colors: [rgb(255, 255, 255), chrome, rgb(90, 95, 110)], atLocations: [0, 0.4, 1], colorSpace: .deviceRGB)!.draw(in: NSBezierPath(ovalIn: ball), relativeCenterPosition: NSPoint(x: -0.4, y: 0.5))
        label("↑↓ SELECT   ⏎ LAUNCH   ESC QUIT", in: NSRect(x: 0, y: h - 34, width: w, height: 18), font: dmdFont(10), color: chrome.withAlphaComponent(0.6))
    }
}

// ---------------------------------------------------------------- app
let app = NSApplication.shared
app.setActivationPolicy(.regular)
let W: CGFloat = 560
let H = Backglass.height(games: games.count, actions: actions.count)
let view = Backglass(games: games, actions: actions, status: status, frame: NSRect(x: 0, y: 0, width: W, height: H))
let win = NSWindow(contentRect: NSRect(x: 0, y: 0, width: W, height: H), styleMask: [.titled, .closable, .fullSizeContentView], backing: .buffered, defer: false)
win.title = "Layover"; win.titlebarAppearsTransparent = true; win.titleVisibility = .hidden
win.appearance = NSAppearance(named: .darkAqua); win.backgroundColor = bgTop; win.isMovableByWindowBackground = true
win.contentView = view; win.center(); win.level = .floating
var result: Int32 = 1
view.onChoose = { name in print(name); fflush(stdout); result = 0; app.stop(nil); win.close() }
view.onCancel = { app.stop(nil); win.close() }
final class Delegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    func windowWillClose(_ n: Notification) { app.stop(nil) }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}
let delegate = Delegate(); app.delegate = delegate; win.delegate = delegate
if let path = snapshot {
    win.orderFront(nil); view.layoutSubtreeIfNeeded()
    let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds)!; view.cacheDisplay(in: view.bounds, to: rep)
    try! rep.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: path))
    print("snapshot \(path)"); exit(0)
}
win.makeKeyAndOrderFront(nil); win.makeFirstResponder(view)
app.activate(ignoringOtherApps: true)
app.run()
exit(result)
