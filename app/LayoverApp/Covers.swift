import AppKit

/// Cover art: local files the backend already found, else a download from Steam's CDN into
/// covers/<appid>/<kind>.jpg (the same layout the Python side reads and writes), else a drawn placeholder.
/// 404s are remembered in covers/<appid>/missing.json for 7 days, like the backend does.
final class CoverStore {
    static let shared = CoverStore()
    enum Kind: String { case capsule, header, hero, logo }

    private let cache = NSCache<NSString, NSImage>()
    private var inflight = Set<String>()
    private var waiters: [String: [(NSImage?) -> Void]] = [:]
    private var missing = Set<String>()
    private let session: URLSession = {
        let c = URLSessionConfiguration.ephemeral
        c.httpMaximumConnectionsPerHost = 4
        c.timeoutIntervalForRequest = 10
        return URLSession(configuration: c)
    }()
    var downloadsEnabled = true
    var onLoaded: ((String) -> Void)?   // appid

    init() { cache.countLimit = 400 }

    /// Synchronous when the image is cached or on disk; otherwise nil now and `completion` later (main thread).
    func image(for game: Game, kind: Kind, completion: ((NSImage?) -> Void)? = nil) -> NSImage? {
        let key = "\(game.appid):\(kind.rawValue)" as NSString
        if let img = cache.object(forKey: key) { return img }
        let local: String?
        switch kind {
        case .capsule: local = game.covers.capsule
        case .header: local = game.covers.header
        case .hero: local = game.covers.hero
        case .logo: local = game.covers.logo
        }
        if let p = local, let img = NSImage(contentsOfFile: p), img.isValid {
            cache.setObject(img, forKey: key)
            return img
        }
        let own = Backend.shared.coversDir + "/\(game.appid)/\(kind.rawValue)" + (kind == .logo ? ".png" : ".jpg")
        if let img = NSImage(contentsOfFile: own), img.isValid {
            cache.setObject(img, forKey: key)
            return img
        }
        guard downloadsEnabled, !game.isMac, !missing.contains(key as String), !isMarkedMissing(game.appid, kind),
              let urlString = game.cover_urls?[kind.rawValue], let url = URL(string: urlString) else { return nil }
        if let c = completion { waiters[key as String, default: []].append(c) }
        if inflight.contains(key as String) { return nil }
        inflight.insert(key as String)
        let dest = own
        session.dataTask(with: url) { [weak self] data, response, _ in
            let code = (response as? HTTPURLResponse)?.statusCode ?? 0
            var image: NSImage?
            if code == 200, let d = data, let img = NSImage(data: d), img.isValid {
                image = img
                let dir = (dest as NSString).deletingLastPathComponent
                try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
                try? d.write(to: URL(fileURLWithPath: dest), options: .atomic)
            }
            DispatchQueue.main.async {
                guard let self = self else { return }
                self.inflight.remove(key as String)
                if let img = image { self.cache.setObject(img, forKey: key) }
                else if code == 404 || code == 403 { self.missing.insert(key as String); self.markMissing(game.appid, kind) }
                let cbs = self.waiters.removeValue(forKey: key as String) ?? []
                cbs.forEach { $0(image) }
                if image != nil { self.onLoaded?(game.appid) }
            }
        }.resume()
        return nil
    }

    private func missingPath(_ appid: String) -> String { Backend.shared.coversDir + "/\(appid)/missing.json" }

    private func isMarkedMissing(_ appid: String, _ kind: Kind) -> Bool {
        guard let d = FileManager.default.contents(atPath: missingPath(appid)),
              let o = try? JSONSerialization.jsonObject(with: d) as? [String: Double], let t = o[kind.rawValue] else { return false }
        return Date().timeIntervalSince1970 - t < 7 * 86400
    }

    private func markMissing(_ appid: String, _ kind: Kind) {
        var o: [String: Double] = [:]
        if let d = FileManager.default.contents(atPath: missingPath(appid)), let e = try? JSONSerialization.jsonObject(with: d) as? [String: Double] { o = e }
        o[kind.rawValue] = Date().timeIntervalSince1970
        let dir = (missingPath(appid) as NSString).deletingLastPathComponent
        try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        if let d = try? JSONSerialization.data(withJSONObject: o) { try? d.write(to: URL(fileURLWithPath: missingPath(appid))) }
    }

    /// A deterministic tile for games without art: two dark hues from the appid plus the name.
    static func placeholder(name: String, appid: String, size: NSSize) -> NSImage {
        let seed = appid.unicodeScalars.reduce(5381) { ($0 << 5) &+ $0 &+ Int($1.value) }
        let hue = CGFloat(abs(seed) % 360) / 360
        let c1 = NSColor(hue: hue, saturation: 0.35, brightness: 0.30, alpha: 1)
        let c2 = NSColor(hue: (hue + 40.0 / 360).truncatingRemainder(dividingBy: 1), saturation: 0.40, brightness: 0.16, alpha: 1)
        let img = NSImage(size: size)
        img.lockFocus()
        NSGradient(starting: c1, ending: c2)?.draw(in: NSRect(origin: .zero, size: size), angle: -60)
        let para = NSMutableParagraphStyle()
        para.lineBreakMode = .byWordWrapping
        let attrs: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: size.width * 0.1, weight: .bold),
                                                    .foregroundColor: NSColor.white.withAlphaComponent(0.9), .paragraphStyle: para]
        let inset = size.width * 0.08
        let rect = NSRect(x: inset, y: inset, width: size.width - 2 * inset, height: size.height * 0.5)
        (name as NSString).draw(with: rect, options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine], attributes: attrs)
        img.unlockFocus()
        return img
    }
}
