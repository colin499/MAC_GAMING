import AppKit

/// Colors, fonts and badge styles shared by every view. Dark only: the window forces darkAqua so the
/// library sits next to games without a white flash.
enum Theme {
    static func rgb(_ r: Int, _ g: Int, _ b: Int, _ a: CGFloat = 1) -> NSColor {
        NSColor(srgbRed: CGFloat(r) / 255, green: CGFloat(g) / 255, blue: CGFloat(b) / 255, alpha: a)
    }
    static let background = rgb(0x16, 0x19, 0x20)
    static let panel = rgb(0x1E, 0x22, 0x2B)
    static let panelRaised = rgb(0x25, 0x2A, 0x35)
    static let separator = NSColor.white.withAlphaComponent(0.08)
    static let textPrimary = NSColor.white.withAlphaComponent(0.92)
    static let textSecondary = NSColor.white.withAlphaComponent(0.60)
    static let textTertiary = NSColor.white.withAlphaComponent(0.38)
    static let accent = rgb(0x4C, 0x8D, 0xFF)
    static let green = rgb(0x3D, 0xBE, 0x6A)
    static let amber = rgb(0xE6, 0xA8, 0x3B)
    static let red = rgb(0xE5, 0x5A, 0x5A)
    static let purple = rgb(0xA7, 0x7B, 0xF2)
    static let blue = rgb(0x4C, 0x8D, 0xFF)
    static let grey = NSColor.white.withAlphaComponent(0.55)

    static let tileCorner: CGFloat = 8
    static let tileWidth: CGFloat = 150
    static let tileCoverHeight: CGFloat = 225
    static let tileHeight: CGFloat = 225 + 6 + 34 + 16   // cover, gap, two name lines, status line
    static let detailWidth: CGFloat = 340

    static func font(_ size: CGFloat, _ weight: NSFont.Weight = .regular) -> NSFont {
        NSFont.systemFont(ofSize: size, weight: weight)
    }

    /// Text style for a badge pill: (fill, text).
    static func badgeColors(_ kind: BadgeKind) -> (NSColor, NSColor) {
        switch kind {
        case .installed: return (green.withAlphaComponent(0.22), green)
        case .notInstalled: return (NSColor.white.withAlphaComponent(0.10), grey)
        case .downloading: return (blue.withAlphaComponent(0.25), blue)
        case .update: return (amber.withAlphaComponent(0.25), amber)
        case .mac: return (purple.withAlphaComponent(0.25), purple)
        case .failed: return (red.withAlphaComponent(0.25), red)
        case .running: return (green.withAlphaComponent(0.35), NSColor.white)
        case .pending: return (blue.withAlphaComponent(0.25), blue)
        }
    }

    static func verdictColor(_ verdict: String) -> NSColor {
        switch verdict {
        case "should_work": return green
        case "varies": return amber
        case "likely_issues": return amber
        case "wont_run": return red
        default: return textTertiary
        }
    }

    static func verdictGlyph(_ verdict: String) -> String {
        switch verdict {
        case "should_work": return "✓"
        case "varies": return "~"
        case "likely_issues": return "!"
        case "wont_run": return "✕"
        default: return "?"
        }
    }
}

enum BadgeKind { case installed, notInstalled, downloading, update, mac, failed, running, pending }

enum Format {
    static let bytes: ByteCountFormatter = {
        let f = ByteCountFormatter()
        f.countStyle = .file
        f.allowedUnits = [.useMB, .useGB, .useTB]
        return f
    }()
    static func size(_ n: Int64) -> String { n > 0 ? bytes.string(fromByteCount: n) : "—" }
    static func gb(_ n: Int64) -> String { String(format: "%.0f GB", Double(n) / 1_000_000_000) }

    static let relative: RelativeDateTimeFormatter = {
        let f = RelativeDateTimeFormatter()
        f.unitsStyle = .full
        return f
    }()
    static func lastPlayed(_ epoch: Double?) -> String {
        guard let e = epoch, e > 0 else { return "Never" }
        return relative.localizedString(for: Date(timeIntervalSince1970: e), relativeTo: Date()).capitalizedFirst
    }
    static func playtime(_ minutes: Int?) -> String {
        guard let m = minutes, m > 0 else { return "" }
        if m < 60 { return "\(m) min" }
        return String(format: "%.1f h", Double(m) / 60)
    }
}

extension String {
    var capitalizedFirst: String { prefix(1).uppercased() + dropFirst() }
}

/// A label with our defaults (no background, not editable, wraps when asked).
func makeLabel(_ text: String = "", size: CGFloat = 13, weight: NSFont.Weight = .regular, color: NSColor = Theme.textPrimary, lines: Int = 1) -> NSTextField {
    let l = NSTextField(labelWithString: text)
    l.font = Theme.font(size, weight)
    l.textColor = color
    l.maximumNumberOfLines = lines
    l.lineBreakMode = lines == 1 ? .byTruncatingTail : .byWordWrapping
    l.cell?.truncatesLastVisibleLine = true
    l.translatesAutoresizingMaskIntoConstraints = false
    return l
}
