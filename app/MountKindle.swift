// Mount Kindle: a menu-bar app that mounts a USB Kindle Colorsoft as /Volumes/Kindle
// by running the bundled kindle-mtpfs helper (macFUSE FSKit backend + libmtp).
// MIT licensed. See LICENSE.
import SwiftUI
import AppKit

let mountPoint = "/Volumes/Kindle"
let settingsPath = "System Settings > General > Login Items & Extensions > File System Extensions > macFUSE"
let logURL = FileManager.default.homeDirectoryForCurrentUser
    .appendingPathComponent("Library/Logs/MountKindle.log")

/// Mask anything that looks like a Kindle serial (G + 3 or more of [0-9A-Z]).
func redact(_ s: String) -> String {
    s.replacingOccurrences(of: "G[0-9A-Z]{3,}", with: "GR…(redacted)", options: .regularExpression)
}

func appendLog(_ s: String) {
    let line = redact(s.hasSuffix("\n") ? s : s + "\n")
    let df = ISO8601DateFormatter(); df.timeZone = .current
    let stamped = line.split(separator: "\n", omittingEmptySubsequences: false)
        .filter { !$0.isEmpty }.map { "\(df.string(from: Date())) \($0)\n" }.joined()
    try? FileManager.default.createDirectory(at: logURL.deletingLastPathComponent(), withIntermediateDirectories: true)
    if let h = try? FileHandle(forWritingTo: logURL) {
        h.seekToEndOfFile(); h.write(stamped.data(using: .utf8)!); try? h.close()
    } else {
        try? stamped.write(to: logURL, atomically: true, encoding: .utf8)
    }
}

/// Run a short command and return (exit code, combined output).
@discardableResult
func run(_ path: String, _ args: [String]) -> (Int32, String) {
    let p = Process(); p.executableURL = URL(fileURLWithPath: path); p.arguments = args
    let pipe = Pipe(); p.standardOutput = pipe; p.standardError = pipe
    do { try p.run() } catch { return (-1, "\(error)") }
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    p.waitUntilExit()
    return (p.terminationStatus, String(decoding: data, as: UTF8.self))
}

func isMounted() -> Bool {
    run("/sbin/mount", []).1.contains(" on \(mountPoint) ")
}

func macFUSEInstalled() -> Bool {
    FileManager.default.fileExists(atPath: "/Library/Filesystems/macfuse.fs")
        && FileManager.default.fileExists(atPath: "/usr/local/lib/libfuse3.4.dylib")
}

func calibreRunning() -> Bool {
    if NSWorkspace.shared.runningApplications.contains(where: {
        ($0.bundleIdentifier ?? "").lowercased().contains("kovidgoyal.calibre")
            || ($0.bundleURL?.lastPathComponent ?? "").lowercased() == "calibre.app" }) { return true }
    return run("/usr/bin/pgrep", ["-f", "/calibre.app/"]).0 == 0
}

/// Look for the Kindle Colorsoft (USB 1949:9981) in the IORegistry. Read-only; doesn't talk to the device.
func kindlePresent() -> Bool {
    let out = run("/usr/sbin/ioreg", ["-p", "IOUSB", "-l", "-w0"]).1
    var vendor = false
    for line in out.split(separator: "\n") {
        if line.contains("+-o ") { vendor = false }
        if line.contains("\"idVendor\" = 6473") { vendor = true }
        if vendor && line.contains("\"idProduct\" = 39297") { return true }
    }
    return false
}

enum Phase: Equatable { case idle, mounting, mounted, unmounting }

@MainActor
final class Model: ObservableObject {
    @Published var phase: Phase = .idle
    @Published var status = "Not mounted"
    @Published var message: String? = nil
    @Published var readOnly = UserDefaults.standard.bool(forKey: "readOnly") {
        didSet { UserDefaults.standard.set(readOnly, forKey: "readOnly") }
    }
    private var helper: Process?
    private var helperOutput = ""
    private var timer: Timer?

    init() {
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { _ in
            Task { @MainActor in self.refresh() }
        }
    }

    var helperPath: String {
        Bundle.main.bundleURL.appendingPathComponent("Contents/Helpers/kindle-mtpfs").path
    }

    func setStatus(_ s: String, message m: String? = nil) {
        status = s; message = m
        appendLog("status: \(s)" + (m.map { " | \($0)" } ?? ""))
    }

    func refresh() {
        guard phase == .idle || phase == .mounted else { return }
        let m = isMounted()
        if m && phase != .mounted { phase = .mounted; status = "Mounted at \(mountPoint)" }
        if !m && phase == .mounted { phase = .idle; status = "Not mounted"; helper = nil }
    }

    func mount() {
        guard phase == .idle else { return }
        if isMounted() { phase = .mounted; setStatus("Mounted at \(mountPoint)"); return }
        if !macFUSEInstalled() {
            setStatus("macFUSE is missing", message: "Install macFUSE 5 from https://macfuse.github.io/ (approve it when macOS asks), then try again."); return
        }
        if calibreRunning() {
            setStatus("Calibre is running", message: "Quit Calibre first. Only one app can use the Kindle at a time."); return
        }
        if !kindlePresent() {
            setStatus("No Kindle found", message: "Plug in the Kindle Colorsoft with a USB data cable and unlock it."); return
        }
        guard FileManager.default.isExecutableFile(atPath: helperPath) else {
            setStatus("App is damaged", message: "The bundled helper is missing. Download the app again."); return
        }
        phase = .mounting; setStatus("Mounting…")
        helperOutput = ""
        let p = Process()
        p.executableURL = URL(fileURLWithPath: helperPath)
        p.arguments = (readOnly ? ["--ro"] : []) + [mountPoint]
        let pipe = Pipe(); p.standardOutput = pipe; p.standardError = pipe
        pipe.fileHandleForReading.readabilityHandler = { h in
            let d = h.availableData
            guard !d.isEmpty else { h.readabilityHandler = nil; return }
            let s = redact(String(decoding: d, as: UTF8.self))
            appendLog(s)
            Task { @MainActor in self.helperOutput += s }
        }
        p.terminationHandler = { proc in
            let code = proc.terminationStatus
            // let the last output chunks land before classifying the exit
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { self.helperExited(code) }
        }
        do { try p.run() } catch {
            phase = .idle; setStatus("Couldn't start the helper", message: "\(error)"); return
        }
        helper = p
        appendLog("started helper pid \(p.processIdentifier) \(readOnly ? "read-only" : "read-write")")
        waitForMount(deadline: Date().addingTimeInterval(90))
    }

    private func waitForMount(deadline: Date) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [self] in
            guard phase == .mounting else { return }
            if isMounted() {
                phase = .mounted
                setStatus("Mounted at \(mountPoint)" + (readOnly ? " (read-only)" : ""))
            } else if Date() > deadline {
                setStatus("Still mounting after 90 s", message: "Check ~/Library/Logs/MountKindle.log. Use Unmount to stop.")
                phase = .mounted   // lets Unmount stop the helper
            } else { waitForMount(deadline: deadline) }
        }
    }

    private func helperExited(_ code: Int32) {
        helper = nil
        let out = helperOutput
        appendLog("helper exited (\(code))")
        if phase == .unmounting || phase == .mounted {
            phase = .idle; setStatus("Not mounted. Kindle released.", message: message); return
        }
        phase = .idle
        if out.contains("extension not enabled") || out.contains("requires approval") {
            setStatus("File system extension is off",
                      message: "Turn it on once in \(settingsPath) (switch on), then Mount again. Don't enable kernel extensions in Recovery Mode; that isn't needed.")
        } else if out.contains("probably busy") || out.contains("LIBUSB_ERROR_BUSY") || out.contains("LIBUSB_ERROR_ACCESS") {
            setStatus("Kindle is busy", message: "Another app is using the Kindle. Quit Calibre, kpush, Android File Transfer or OpenMTP, then try again.")
        } else if out.contains("no Kindle") {
            setStatus("No Kindle found", message: "Plug in the Kindle Colorsoft and unlock it.")
        } else {
            let tail = out.split(separator: "\n").suffix(3).joined(separator: "\n")
            setStatus("Mount failed", message: tail.isEmpty ? "exit code \(code)" : tail)
        }
    }

    func unmount(then done: (() -> Void)? = nil) {
        guard phase == .mounted || phase == .mounting else { done?(); return }
        phase = .unmounting; setStatus("Unmounting…")
        let p = helper
        DispatchQueue.global().async {
            var r = run("/sbin/umount", [mountPoint])
            if r.0 != 0 && isMounted() { r = run("/usr/sbin/diskutil", ["unmount", mountPoint]) }
            if let p = p {
                for _ in 0..<30 where p.isRunning { Thread.sleep(forTimeInterval: 0.5) }
                if p.isRunning { p.terminate(); for _ in 0..<20 where p.isRunning { Thread.sleep(forTimeInterval: 0.5) } }
            }
            let still = isMounted()
            let out = redact(r.1)
            DispatchQueue.main.async {
                if still {
                    self.phase = .mounted
                    self.setStatus("Couldn't unmount", message: "Close any Finder windows or apps using the Kindle and try again. " + out)
                } else {
                    self.phase = .idle; self.helper = nil
                    self.setStatus("Not mounted. Kindle released.")
                }
                done?()
            }
        }
    }

    func openInFinder() { NSWorkspace.shared.open(URL(fileURLWithPath: mountPoint)) }

    func quit() {
        if phase == .mounted || phase == .mounting { unmount { NSApp.terminate(nil) } } else { NSApp.terminate(nil) }
    }
}

/// mountkindle://mount, mountkindle://unmount, mountkindle://quit (for scripts and agents).
final class AppDelegate: NSObject, NSApplicationDelegate {
    var model: Model?
    func applicationDidFinishLaunching(_ n: Notification) {
        NSAppleEventManager.shared().setEventHandler(self, andSelector: #selector(handleURL(_:reply:)),
            forEventClass: AEEventClass(kInternetEventClass), andEventID: AEEventID(kAEGetURL))
        appendLog("Mount Kindle started")
    }
    @objc func handleURL(_ e: NSAppleEventDescriptor, reply: NSAppleEventDescriptor) {
        guard let s = e.paramDescriptor(forKeyword: keyDirectObject)?.stringValue, let u = URL(string: s) else { return }
        Task { @MainActor in
            guard let m = self.model else { return }
            switch u.host ?? "" {
            case "mount": m.mount()
            case "unmount": m.unmount()
            case "quit": m.quit()
            default: break
            }
        }
    }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        // Release the Kindle if the app is quit while mounted (for example at logout).
        if isMounted() { _ = run("/sbin/umount", [mountPoint]) }
        return .terminateNow
    }
}

@main
struct MountKindleApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate
    @StateObject private var model = Model()

    var body: some Scene {
        MenuBarExtra {
            MenuContent(model: model).onAppear { delegate.model = model }
        } label: {
            Image(systemName: model.phase == .mounted ? "book.fill" : "book")
                .onAppear { delegate.model = model }
        }
        .menuBarExtraStyle(.menu)
    }
}

struct MenuContent: View {
    @ObservedObject var model: Model
    var body: some View {
        Text(model.status)
        if let m = model.message { Text(m) }
        Divider()
        Button("Mount") { model.mount() }.disabled(model.phase != .idle)
        Button("Unmount") { model.unmount() }.disabled(model.phase != .mounted)
        Button("Open in Finder") { model.openInFinder() }.disabled(model.phase != .mounted)
        Toggle("Read-only", isOn: $model.readOnly).disabled(model.phase != .idle)
        Divider()
        Text(model.readOnly ? "Read-only: changes are refused" : "Read-write: deletes on the Kindle are permanent")
        Button("Quit") { model.quit() }.keyboardShortcut("q")
    }
}
