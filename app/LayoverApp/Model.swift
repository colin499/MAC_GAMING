import Foundation

/// The document `layover library --json` prints (schema 1). Every field the app does not strictly
/// need is optional so a backend a version ahead or behind still decodes.
struct Library: Decodable {
    var schema: Int?
    var generated: String?
    var layover: LayoverInfo
    var appcache: AppcacheStatus?
    var steam: SteamState
    var disk: Disk
    var counts: Counts
    var games: [Game]
}

struct LayoverInfo: Decodable {
    var version: String?
    var engine: String?
    var engine_ready: Bool
    var steam_installed: Bool
    var root: String
    var covers_dir: String?
    var repo: String?
    var renderer_default: String?
    var fps_cap_default: Int?
}

struct AppcacheStatus: Decodable {
    var appinfo_error: String?
    var packageinfo_error: String?
}

struct SteamState: Decodable {
    var running: Bool
    var native_running: Bool?
    var connection: String?
    var offline: Bool
    var running_renderer: String?
    var running_retina: Bool?
    var running_game: String?
    var launch_prompt_appid: String?
    var accounts: [Account]?
}

struct Account: Decodable {
    var steamid: String?
    var account_name: String?
    var persona: String?
    var most_recent: Bool?
}

struct Disk: Decodable {
    var path: String?
    var free: Int64
    var total: Int64
}

struct Counts: Decodable {
    var owned: Int
    var installed: Int
    var free: Int
    var downloading: Int
}

struct Covers: Decodable {
    var capsule: String?
    var header: String?
    var hero: String?
    var logo: String?
    var icon: String?
    var hero_blur: String?
}

struct Compat: Decodable {
    var verdict: String
    var label: String
    var reasons: [String]?
    var unverified: Bool?
    var checked: Double?
}

struct Effective: Decodable {
    var renderer: String?
    var fps_cap: Int?
    var retina: Bool?
    var metalfx: Double?
}

struct ProfileInfo: Decodable {
    var available: Bool?
    var applied: Bool?
    var version: String?
    var pending_ini: Bool?
    var notes: [String]?
}

struct LaunchEntry: Decodable {
    var exe: String?
    var args: String?
    var description: String?
}

/// Any JSON value (the per-game settings dict mixes strings, numbers, bools and an env dict).
enum JSONValue: Decodable, CustomStringConvertible {
    case string(String), number(Double), bool(Bool), null, array([JSONValue]), object([String: JSONValue])

    init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if c.decodeNil() { self = .null }
        else if let b = try? c.decode(Bool.self) { self = .bool(b) }
        else if let n = try? c.decode(Double.self) { self = .number(n) }
        else if let s = try? c.decode(String.self) { self = .string(s) }
        else if let a = try? c.decode([JSONValue].self) { self = .array(a) }
        else if let o = try? c.decode([String: JSONValue].self) { self = .object(o) }
        else { self = .null }
    }

    var description: String {
        switch self {
        case .string(let s): return s
        case .number(let n): return n == n.rounded() ? String(Int(n)) : String(n)
        case .bool(let b): return b ? "on" : "off"
        case .null: return "default"
        case .array(let a): return a.map { $0.description }.joined(separator: " ")
        case .object(let o): return o.map { "\($0.key)=\($0.value)" }.sorted().joined(separator: " ")
        }
    }
}

struct Game: Decodable {
    var appid: String
    var name: String
    var type: String?
    var `where`: String?
    var owned: Bool
    var source: [String]?
    var installed: Bool
    var ready: Bool
    var needs_update: Bool
    var state_flags: Int?
    var state: [String]?
    var downloading: Bool
    var paused: Bool?
    var progress: Double
    var size: Int64
    var bytes_to_download: Int64?
    var bytes_downloaded: Int64?
    var install_path: String?
    var last_played: Double?
    var playtime_min: Int?
    var running: Bool?
    var renderer: String
    var renderer_source: String?
    var dx12: Bool?
    var settings: [String: JSONValue]?
    var effective: Effective?
    var profile: ProfileInfo?
    var covers: Covers
    var cover_urls: [String: String]?
    var compat: Compat
    var launch: [LaunchEntry]?
    var launch_options: String?
    var controller_support: String?
    var deck_category: Int?
    var developer: String?
    var publisher: String?
    var dlc_owned: [String]?

    var isMac: Bool { `where` == "mac" }
    var isRunning: Bool { running ?? false }

    /// What the big button does right now.
    enum Primary { case play, install, update, downloading, running }
    var primary: Primary {
        if downloading { return .downloading }
        if isRunning { return .running }
        if installed && needs_update { return .update }
        if installed { return .play }
        return .install
    }
}

/// A click the user made that the backend has acknowledged but Steam has not yet reflected in its files.
struct PendingAction {
    enum Kind { case install, remove, play, update }
    var kind: Kind
    var started: Date
    var message: String
    var timeout: TimeInterval { kind == .play ? 60 : 120 }
    var expired: Bool { Date().timeIntervalSince(started) > timeout }
}

/// Filter and sort state for the grid.
enum LibraryFilter: Int { case all = 0, installed, notInstalled }
enum LibrarySort: Int { case installedFirst = 0, name, size, lastPlayed }

enum Sorting {
    static func apply(_ games: [Game], filter: LibraryFilter, sort: LibrarySort, search: String) -> [Game] {
        let q = search.trimmingCharacters(in: .whitespaces).lowercased()
        var list = games.filter { g in
            switch filter {
            case .all: return true
            case .installed: return g.installed
            case .notInstalled: return !g.installed
            }
        }
        if !q.isEmpty { list = list.filter { $0.name.lowercased().contains(q) || $0.appid == q } }
        let byName: (Game, Game) -> Bool = { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
        switch sort {
        case .installedFirst:
            list.sort { a, b in
                if a.installed != b.installed { return a.installed }
                if a.owned != b.owned { return a.owned }
                return byName(a, b)
            }
        case .name: list.sort(by: byName)
        case .size: list.sort { a, b in a.size != b.size ? a.size > b.size : byName(a, b) }
        case .lastPlayed: list.sort { a, b in (a.last_played ?? 0) != (b.last_played ?? 0) ? (a.last_played ?? 0) > (b.last_played ?? 0) : byName(a, b) }
        }
        return list
    }
}
