// Renders Layover's icon (a plane on a night-sky rounded square) to an .icns. Run: swift app/make_icon.swift app/Layover.icns
import AppKit
let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "Layover.icns"
let tmp = NSTemporaryDirectory() + "layover.iconset"; try? FileManager.default.removeItem(atPath: tmp); try! FileManager.default.createDirectory(atPath: tmp, withIntermediateDirectories: true)
func render(_ px: Int) -> Data {
    let img = NSImage(size: NSSize(width: px, height: px)); img.lockFocus()
    let r = NSRect(x: 0, y: 0, width: px, height: px).insetBy(dx: CGFloat(px) * 0.04, dy: CGFloat(px) * 0.04)
    let path = NSBezierPath(roundedRect: r, xRadius: CGFloat(px) * 0.22, yRadius: CGFloat(px) * 0.22)
    NSGradient(starting: NSColor(calibratedRed: 0.10, green: 0.16, blue: 0.38, alpha: 1), ending: NSColor(calibratedRed: 0.02, green: 0.05, blue: 0.14, alpha: 1))!.draw(in: path, angle: -90)
    let p = NSMutableParagraphStyle(); p.alignment = .center
    let s = NSAttributedString(string: "✈️", attributes: [.font: NSFont.systemFont(ofSize: CGFloat(px) * 0.62), .paragraphStyle: p])
    let sz = s.size(); s.draw(in: NSRect(x: 0, y: (CGFloat(px) - sz.height) / 2 + CGFloat(px) * 0.02, width: CGFloat(px), height: sz.height))
    img.unlockFocus()
    let rep = NSBitmapImageRep(data: img.tiffRepresentation!)!; rep.size = NSSize(width: px, height: px)
    return rep.representation(using: .png, properties: [:])!
}
for (name, px) in [("icon_16x16", 16), ("icon_16x16@2x", 32), ("icon_32x32", 32), ("icon_32x32@2x", 64), ("icon_128x128", 128), ("icon_128x128@2x", 256), ("icon_256x256", 256), ("icon_256x256@2x", 512), ("icon_512x512", 512), ("icon_512x512@2x", 1024)] {
    try! render(px).write(to: URL(fileURLWithPath: tmp + "/" + name + ".png"))
}
let t = Process(); t.launchPath = "/usr/bin/iconutil"; t.arguments = ["-c", "icns", tmp, "-o", out]; t.launch(); t.waitUntilExit(); print("wrote \(out) rc \(t.terminationStatus)")
