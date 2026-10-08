import Cocoa
// usage: wincap list | wincap <windowid> out.png | wincap name <substring> out.png
let args = CommandLine.arguments
let list = CGWindowListCopyWindowInfo([.optionAll, .excludeDesktopElements], kCGNullWindowID) as! [[String: Any]]
func save(_ wid: CGWindowID, _ path: String) {
    let t = Process(); t.launchPath = "/usr/sbin/screencapture"; t.arguments = ["-x", "-o", "-l", String(wid), path]; t.launch(); t.waitUntilExit(); print("screencapture rc \(t.terminationStatus) -> \(path)")
}
if args.count < 2 || args[1] == "list" {
    for w in list {
        let wid = w[kCGWindowNumber as String] as! Int; let owner = w[kCGWindowOwnerName as String] as? String ?? "?"
        let name = w[kCGWindowName as String] as? String ?? ""; let b = w[kCGWindowBounds as String] as? [String: Any] ?? [:]
        let layer = w[kCGWindowLayer as String] as? Int ?? 0; let onscreen = w[kCGWindowIsOnscreen as String] as? Bool ?? false
        print("\(wid)\t\(owner)\t'\(name)'\t\(b["Width"] ?? 0)x\(b["Height"] ?? 0)@\(b["X"] ?? 0),\(b["Y"] ?? 0)\tlayer \(layer) onscreen \(onscreen)")
    }
} else if args[1] == "name" {
    for w in list { let name = w[kCGWindowName as String] as? String ?? ""; let owner = w[kCGWindowOwnerName as String] as? String ?? ""
        if name.contains(args[2]) || owner.contains(args[2]) { let b = w[kCGWindowBounds as String] as? [String: Any] ?? [:]; if (b["Width"] as? Int ?? 0) > 400 { save(CGWindowID(w[kCGWindowNumber as String] as! Int), args[3]); exit(0) } } }
    print("no window"); exit(1)
} else { save(CGWindowID(Int(args[1])!), args[2]) }
