import AppKit

// Layover's library window. Usage: LayoverApp [--snapshot out.png] [--snapshot-detail out.png] [--mock library.json]
//   [--select appid] [--search text] [--filter all|installed|missing] [--sort name|size|played]
//   [--wait-covers seconds] [--script path] [--python path] [--pending appid:install|remove|play|update] [--dry-run]
let options = Options.parse(CommandLine.arguments)
let app = NSApplication.shared
app.setActivationPolicy(options.snapshot != nil || options.snapshotDetail != nil ? .accessory : .regular)
let delegate = AppDelegate(options: options)
app.delegate = delegate
app.run()
