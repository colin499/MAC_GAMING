import AppKit

/// Headless rendering: the only way an agent session can see the window (no screen recording permission).
/// `--snapshot out.png` renders the whole window, `--snapshot-detail out.png` the detail pane at 2x.
enum Snapshot {
    static func run(window: NSWindow, controller: LibraryViewController, options: Options) {
        let wait = options.waitCovers ?? (options.mock == nil ? 5.0 : 0.5)
        window.orderFront(nil)
        controller.view.layoutSubtreeIfNeeded()
        controller.collection.collectionViewLayout?.invalidateLayout()
        controller.collection.reloadData()
        if let s = options.select { controller.select(appid: s) }
        controller.collection.layoutSubtreeIfNeeded()
        RunLoop.main.run(until: Date(timeIntervalSinceNow: wait))
        controller.reload()
        controller.view.layoutSubtreeIfNeeded()
        RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.3))
        if let path = options.snapshot {
            let v = controller.view
            guard let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds) else { print("snapshot failed"); exit(2) }
            v.cacheDisplay(in: v.bounds, to: rep)
            write(rep, to: path)
            let sel = controller.selectedGame()?.name ?? "-"
            print("snapshot \(path) tiles=\(controller.visible.count) selected=\(sel) pending=\(Backend.shared.pending.count)")
        }
        if let path = options.snapshotDetail {
            let v = controller.detail
            let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: Int(v.bounds.width) * 2, pixelsHigh: Int(v.bounds.height) * 2, bitsPerSample: 8,
                                       samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
            rep.size = v.bounds.size
            v.cacheDisplay(in: v.bounds, to: rep)
            write(rep, to: path)
            print("snapshot-detail \(path)")
        }
        exit(0)
    }

    private static func write(_ rep: NSBitmapImageRep, to path: String) {
        guard let data = rep.representation(using: .png, properties: [:]) else { print("png failed"); exit(2) }
        do { try data.write(to: URL(fileURLWithPath: path)) } catch { print("write failed: \(error)"); exit(2) }
    }
}
