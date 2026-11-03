import AppKit

/// Talks to the Python launcher: `layover library --json` for state, verb subcommands for actions. The app
/// owns the window and timers; every call is one short-lived subprocess on a background queue.
final class Backend {
    static let shared = Backend()

    var script: String = ""
    var python: String = ""
    var mockPath: String?
    var dryRun = false

    private(set) var library: Library?
    private(set) var lastError: (message: String, details: String)?
    private(set) var pending: [String: PendingAction] = [:]
    private(set) var failed: [String: String] = [:]      // appid -> last failure message
    private(set) var lastRefresh: Date?
    private(set) var busy = 0                           // running subprocesses

    var onUpdate: (() -> Void)?
    var onError: ((String, String) -> Void)?
    var onBusyChange: (() -> Void)?

    private var refreshing = false
    private var refreshAgain = false
    private var timer: Timer?
    private let queue = DispatchQueue(label: "layover.backend", qos: .userInitiated, attributes: .concurrent)

    var rootDir: String { library?.layover.root ?? (NSHomeDirectory() + "/Library/Application Support/Layover") }
    var coversDir: String { library?.layover.covers_dir ?? (rootDir + "/covers") }
    var logPath: String { rootDir + "/logs/app.log" }

    // MARK: locating the launcher

    /// Flags > environment (set by the bundle's zsh shim) > Info.plist keys written by `layover app` > the repo path.
    func locate(scriptFlag: String?, pythonFlag: String?) {
        let env = ProcessInfo.processInfo.environment
        let plist = Bundle.main.infoDictionary ?? [:]
        script = scriptFlag ?? env["LAYOVER_SCRIPT"] ?? (plist["LayoverScript"] as? String)
            ?? (NSHomeDirectory() + "/Documents/BRAIN_FOOD/MAC_GAMING/layover")
        python = pythonFlag ?? env["LAYOVER_PYTHON"] ?? (plist["LayoverPython"] as? String) ?? ""
        if python.isEmpty || !FileManager.default.isExecutableFile(atPath: python) {
            python = ["/opt/homebrew/bin/python3", "/usr/local/bin/python3", "/usr/bin/python3"].first { FileManager.default.isExecutableFile(atPath: $0) } ?? "/usr/bin/python3"
        }
    }

    var scriptAvailable: Bool { FileManager.default.isReadableFile(atPath: script) }

    // MARK: running the launcher

    struct Result { let status: Int32; let stdout: String; let stderr: String; let seconds: Double }

    /// Run `layover <args>` off the main thread; completion on main. Timeout kills the process.
    func run(_ args: [String], timeout: TimeInterval = 60, completion: @escaping (Result) -> Void) {
        busy += 1; onBusyChange?()
        let started = Date()
        if dryRun && args.first != "library" {
            appendLog("$ layover " + args.joined(separator: " ") + "   (dry run)")
            busy -= 1; onBusyChange?()
            completion(Result(status: 0, stdout: "{\"ok\": true, \"dry_run\": true}", stderr: "", seconds: 0))
            return
        }
        queue.async { [self] in
            let p = Process()
            p.executableURL = URL(fileURLWithPath: python)
            p.arguments = [script] + args
            var env = ProcessInfo.processInfo.environment
            env["PYTHONIOENCODING"] = "utf-8"
            env["PATH"] = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin"
            p.environment = env
            let out = Pipe(), err = Pipe()
            p.standardOutput = out; p.standardError = err; p.standardInput = FileHandle.nullDevice
            var outData = Data(), errData = Data()
            let group = DispatchGroup()
            do { try p.run() } catch {
                DispatchQueue.main.async {
                    self.busy -= 1; self.onBusyChange?()
                    completion(Result(status: -1, stdout: "", stderr: "could not start \(self.python): \(error.localizedDescription)", seconds: 0))
                }
                return
            }
            group.enter(); self.queue.async { outData = out.fileHandleForReading.readDataToEndOfFile(); group.leave() }
            group.enter(); self.queue.async { errData = err.fileHandleForReading.readDataToEndOfFile(); group.leave() }
            let killer = DispatchWorkItem { if p.isRunning { p.terminate() } }
            self.queue.asyncAfter(deadline: .now() + timeout, execute: killer)
            p.waitUntilExit()
            killer.cancel()
            group.wait()
            let r = Result(status: p.terminationStatus, stdout: String(decoding: outData, as: UTF8.self),
                           stderr: String(decoding: errData, as: UTF8.self), seconds: Date().timeIntervalSince(started))
            DispatchQueue.main.async {
                self.busy -= 1; self.onBusyChange?()
                if args.first != "library" {
                    self.appendLog(String(format: "$ layover %@ -> rc %d (%.1f s)%@", args.joined(separator: " "), r.status, r.seconds,
                                          r.stderr.isEmpty ? "" : "\n" + r.stderr.trimmingCharacters(in: .whitespacesAndNewlines)))
                }
                completion(r)
            }
        }
    }

    func appendLog(_ line: String) {
        let stamp = ISO8601DateFormatter().string(from: Date())
        let text = "[\(stamp)] \(line)\n"
        let path = logPath
        queue.async {
            try? FileManager.default.createDirectory(atPath: (path as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
            if let h = FileHandle(forWritingAtPath: path) { h.seekToEndOfFile(); h.write(text.data(using: .utf8)!); h.closeFile() }
            else { try? text.write(toFile: path, atomically: true, encoding: .utf8) }
        }
    }

    // MARK: state

    /// Reload the library document. Never two at once; a request during a run queues one more.
    func refresh(completion: (() -> Void)? = nil) {
        if refreshing { refreshAgain = true; return }
        refreshing = true
        let done: (Library?, String?, String?) -> Void = { [self] lib, message, details in
            refreshing = false
            if let lib = lib {
                library = lib
                lastRefresh = Date()
                reconcilePending(with: lib)
                if lastError != nil && message == nil { lastError = nil }
            }
            if let m = message { report(m, details ?? "") }
            onUpdate?()
            completion?()
            scheduleNext()
            if refreshAgain { refreshAgain = false; refresh() }
        }
        if let mock = mockPath {
            do {
                let data = try Data(contentsOf: URL(fileURLWithPath: mock))
                done(try JSONDecoder().decode(Library.self, from: data), nil, nil)
            } catch { done(nil, "Could not read the mock library", "\(error)") }
            return
        }
        guard scriptAvailable else {
            done(nil, "Cannot find the layover script. Run ./layover app from the MAC_GAMING folder.", script)
            return
        }
        run(["library", "--json", "--include-free"], timeout: 25) { r in
            guard r.status == 0, let data = r.stdout.data(using: .utf8), !r.stdout.isEmpty else {
                done(nil, "layover library failed (rc \(r.status))", r.stderr.isEmpty ? r.stdout : r.stderr)
                return
            }
            do { done(try JSONDecoder().decode(Library.self, from: data), nil, nil) }
            catch { done(nil, "Could not decode the library document", "\(error)\n" + String(r.stdout.prefix(400))) }
        }
    }

    private func scheduleNext() {
        timer?.invalidate()
        let active = (library?.games.contains { $0.downloading } ?? false) || !pending.isEmpty || library?.steam.launch_prompt_appid != nil
        timer = Timer.scheduledTimer(withTimeInterval: active ? 2 : 10, repeats: false) { [weak self] _ in self?.refresh() }
    }

    func game(_ appid: String) -> Game? { library?.games.first { $0.appid == appid } }

    private func reconcilePending(with lib: Library) {
        for (appid, action) in pending {
            let g = lib.games.first { $0.appid == appid }
            var clear = action.expired
            switch action.kind {
            case .install, .update: if let g = g, g.downloading || (g.installed && g.ready && action.kind == .install) { clear = true }
            case .remove: if g == nil || !(g!.installed) { clear = true }
            case .play: if let g = g, g.isRunning || lib.steam.running_game == appid { clear = true }
            }
            if clear {
                if action.expired {
                    let what: String
                    switch action.kind {
                    case .install, .update: what = "Steam did not start the download (cancelled in Steam?)"
                    case .remove: what = "Steam did not remove it (cancelled in Steam?)"
                    case .play: what = "The game did not appear to start; look at Steam's window"
                    }
                    failed[appid] = what
                }
                pending.removeValue(forKey: appid)
            }
        }
    }

    private func report(_ message: String, _ details: String) {
        lastError = (message, details)
        appendLog("ERROR " + message + (details.isEmpty ? "" : " | " + details.prefix(400)))
        onError?(message, details)
    }

    func clearError() { lastError = nil; onUpdate?() }
    func setPending(_ appid: String, _ p: PendingAction) { pending[appid] = p; onUpdate?() }
    func clearFailure(_ appid: String) { failed.removeValue(forKey: appid); onUpdate?() }

    // MARK: actions

    private func act(_ appid: String, _ kind: PendingAction.Kind, message: String, args: [String], timeout: TimeInterval) {
        failed.removeValue(forKey: appid)
        pending[appid] = PendingAction(kind: kind, started: Date(), message: message)
        onUpdate?()
        scheduleNext()
        run(args, timeout: timeout) { [self] r in
            if r.status != 0 {
                pending.removeValue(forKey: appid)
                let msg = Self.errorMessage(r)
                failed[appid] = msg
                report("\(args.first ?? "action") failed: \(msg)", r.stderr)
            } else if let m = Self.field(r.stdout, "message"), !m.isEmpty, var p = pending[appid] {
                p.message = m; pending[appid] = p
            }
            onUpdate?()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { self.refresh() }
        }
    }

    func play(_ appid: String) {
        let steamOff = !(library?.steam.running ?? false)
        act(appid, .play, message: steamOff ? "Starting Steam first (about 20 s)…" : "Launching…",
            args: ["play", appid, "--json"], timeout: 150)
    }
    func install(_ appid: String) {
        let steamOff = !(library?.steam.running ?? false)
        act(appid, .install, message: steamOff ? "Starting Steam (about 20 s)…" : "Waiting for Steam…",
            args: ["install", appid, "--json"], timeout: 60)
    }
    func update(_ appid: String) {
        act(appid, .update, message: "Asking Steam to update…", args: ["install", appid, "--json"], timeout: 60)
    }
    func remove(_ appid: String) {
        act(appid, .remove, message: "Removing… confirm in Steam", args: ["remove", appid, "--yes", "--json"], timeout: 60)
    }
    func openSteam(completion: ((Result) -> Void)? = nil) { simple(["steam"], timeout: 60, completion) }
    func quitSteam(completion: ((Result) -> Void)? = nil) { simple(["kill"], timeout: 60, completion) }
    func setFlight(offline: Bool, completion: ((Result) -> Void)? = nil) {
        simple(["flight", offline ? "--set-offline" : "--online"], timeout: 90, completion)
    }
    func setSetting(_ appid: String, key: String, value: String, completion: ((Result) -> Void)? = nil) {
        simple(["game", appid, key, value, "--json"], timeout: 30, completion)
    }
    func resetProfile(_ appid: String, completion: ((Result) -> Void)? = nil) {
        simple(["profile", appid, "--reset", "--json"], timeout: 60, completion)
    }
    func checkCompat(_ appid: String, completion: ((Result) -> Void)? = nil) {
        simple(["compat", appid, "--refresh", "--json"], timeout: 120, completion)
    }

    private func simple(_ args: [String], timeout: TimeInterval, _ completion: ((Result) -> Void)?) {
        run(args, timeout: timeout) { [self] r in
            if r.status != 0 { report("\(args.joined(separator: " ")) failed: \(Self.errorMessage(r))", r.stderr) }
            completion?(r)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { self.refresh() }
        }
    }

    static func field(_ json: String, _ key: String) -> String? {
        guard let data = json.data(using: .utf8), let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        if let s = obj[key] as? String { return s }
        if let v = obj[key] { return "\(v)" }
        return nil
    }

    static func errorMessage(_ r: Result) -> String {
        if let e = field(r.stdout, "error") { return e }
        let tail = r.stderr.split(separator: "\n").last.map(String.init) ?? ""
        return tail.isEmpty ? "exit code \(r.status)" : tail.trimmingCharacters(in: .whitespaces)
    }
}
