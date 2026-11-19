import CoreGraphics
import CoreMedia
import CoreVideo
import CryptoKit
import Darwin
import Foundation
import ImageIO
import ScreenCaptureKit
import UniformTypeIdentifiers

// Reads ./credentials.json automatically if present
let defaultClientId = ""
let defaultClientSecret = ""
let driveScope = "https://www.googleapis.com/auth/drive.file"
let defaultDriveFolderName = "Snap Lecture Captures"

// MARK: - CLI Options

struct Options {
    var diffThresholdPercent: Double = 0.5
    var once = false
    var displayNumber: Int? = nil
    var format = "jpg"
    var outputDir = "captures"
    var localOnly = false
    var driveFolderId = ""
    var driveFolderName = defaultDriveFolderName
    var doLogin = false
    var doLogout = false
    var showHelp = false
}

func printUsage() {
    let usage = """
    snap - Event-driven screen capture to Google Drive (OAuth) for lectures (macOS)

    USAGE:
      ./snap [OPTIONS]

    DEFAULT:
      On first launch, opens browser for Google Drive sign-in.
      Uses macOS ScreenCaptureKit (SCStream) OS events: captures 1 initial shot,
      then macOS only delivers a frame when the screen actually changes (cursor hidden).
      If offline, queues in ./pending_upload/ and deletes each file after upload.
      Press SPACE or ENTER for an immediate manual shot, 'q' or Ctrl+C to stop.

    GOOGLE DRIVE / OAUTH:
      --login                Sign in with Google OAuth (opens browser)
      --logout               Remove saved Google OAuth credentials
      --folder ID            Google Drive folder ID to upload into (optional)
      --folder-name NAME     Auto-created Drive folder name (default: Snap Lecture Captures)
      --local                Save locally to disk instead of uploading to Drive

    CAPTURE OPTIONS:
      --diff PERCENT         Min screen change % to upload (default: 0.5, 0 = all OS events)
      -d, --display NUM      Display number (1 = main, 2 = secondary, ...)
      -f, --format FMT       Image format: jpg or png (default: jpg)
      -o, --out DIR          Local output folder (with --local, default: ./captures)
      -1, --once             Capture/upload a single screenshot and exit
      -h, --help             Show this help message
    """
    print(usage)
}

func parseArgs(_ args: [String]) -> Options? {
    var opts = Options()
    var i = 1
    while i < args.count {
        let arg = args[i]
        switch arg {
        case "-h", "--help":
            opts.showHelp = true
            return opts
        case "-1", "--once":
            opts.once = true
        case "--local":
            opts.localOnly = true
        case "--login":
            opts.doLogin = true
        case "--logout":
            opts.doLogout = true
        case "--folder":
            i += 1
            guard i < args.count, !args[i].isEmpty else {
                fputs("Error: --folder requires a Drive folder ID.\n", stderr)
                return nil
            }
            opts.driveFolderId = args[i]
        case "--folder-name":
            i += 1
            guard i < args.count, !args[i].isEmpty else {
                fputs("Error: --folder-name requires a name.\n", stderr)
                return nil
            }
            opts.driveFolderName = args[i]
        case "--diff":
            i += 1
            guard i < args.count, let val = Double(args[i]), val >= 0, val <= 100 else {
                fputs("Error: --diff must be between 0 and 100.\n", stderr)
                return nil
            }
            opts.diffThresholdPercent = val
        case "-d", "--display":
            i += 1
            guard i < args.count, let val = Int(args[i]), val >= 1 else {
                fputs("Error: --display requires a display number >= 1.\n", stderr)
                return nil
            }
            opts.displayNumber = val
        case "-f", "--format":
            i += 1
            guard i < args.count else {
                fputs("Error: --format requires 'jpg' or 'png'.\n", stderr)
                return nil
            }
            let fmt = args[i].lowercased()
            guard fmt == "jpg" || fmt == "png" else {
                fputs("Error: --format must be 'jpg' or 'png'.\n", stderr)
                return nil
            }
            opts.format = fmt
        case "-o", "--out":
            i += 1
            guard i < args.count, !args[i].isEmpty else {
                fputs("Error: --out requires a directory path.\n", stderr)
                return nil
            }
            opts.outputDir = args[i]
        default:
            fputs("Error: Unknown option '\(arg)'. Use --help for usage.\n", stderr)
            return nil
        }
        i += 1
    }
    return opts
}

// MARK: - Auth & Google Drive Client

struct AuthData: Codable {
    var clientId: String
    var clientSecret: String
    var refreshToken: String
    var driveFolderId: String
}

final class DriveClient {
    var auth: AuthData
    private let authFileURL: URL
    private var accessToken: String = ""
    private var accessTokenExpiry: Date = .distantPast

    init(auth: AuthData, authFileURL: URL) {
        self.auth = auth
        self.authFileURL = authFileURL
    }

    static func loadCredentialsJsonIfPresent(at url: URL) -> (String, String)? {
        guard let data = try? Data(contentsOf: url),
              let obj = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            return nil
        }
        let inner = (obj["installed"] as? [String: Any]) ?? (obj["web"] as? [String: Any]) ?? obj
        guard let cid = inner["client_id"] as? String, !cid.isEmpty else { return nil }
        let csec = (inner["client_secret"] as? String) ?? ""
        return (cid, csec)
    }

    static func loadAuth(from url: URL) -> AuthData? {
        guard let data = try? Data(contentsOf: url),
              let decoded = try? JSONDecoder().decode(AuthData.self, from: data),
              !decoded.clientId.isEmpty, !decoded.refreshToken.isEmpty else {
            return nil
        }
        return decoded
    }

    func saveAuth() throws {
        let data = try JSONEncoder().encode(auth)
        try data.write(to: authFileURL, options: .atomic)
        try? FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: authFileURL.path)
    }

    func performOAuthLogin() -> Bool {
        var rnd = [UInt8](repeating: 0, count: 32)
        _ = SecRandomCopyBytes(kSecRandomDefault, rnd.count, &rnd)
        let verifier = base64UrlEncode(Data(rnd))
        let digest = SHA256.hash(data: Data(verifier.utf8))
        let challenge = base64UrlEncode(Data(digest))

        let listenSock = socket(AF_INET, SOCK_STREAM, 0)
        guard listenSock >= 0 else {
            fputs("Error: Failed to create local socket.\n", stderr)
            return false
        }
        defer { close(listenSock) }

        var addr = sockaddr_in()
        addr.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        addr.sin_family = sa_family_t(AF_INET)
        addr.sin_port = 0
        addr.sin_addr.s_addr = inet_addr("127.0.0.1")

        let bindRes = withUnsafePointer(to: &addr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(listenSock, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard bindRes == 0, listen(listenSock, 1) == 0 else {
            fputs("Error: Failed to bind local loopback port.\n", stderr)
            return false
        }

        var boundAddr = sockaddr_in()
        var addrLen = socklen_t(MemoryLayout<sockaddr_in>.size)
        _ = withUnsafeMutablePointer(to: &boundAddr) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                getsockname(listenSock, $0, &addrLen)
            }
        }
        let port = Int(UInt16(bigEndian: boundAddr.sin_port))
        let redirectUri = "http://127.0.0.1:\(port)"

        var comps = URLComponents(string: "https://accounts.google.com/o/oauth2/v2/auth")!
        comps.queryItems = [
            URLQueryItem(name: "client_id", value: auth.clientId),
            URLQueryItem(name: "redirect_uri", value: redirectUri),
            URLQueryItem(name: "response_type", value: "code"),
            URLQueryItem(name: "scope", value: driveScope),
            URLQueryItem(name: "access_type", value: "offline"),
            URLQueryItem(name: "prompt", value: "consent"),
            URLQueryItem(name: "code_challenge", value: challenge),
            URLQueryItem(name: "code_challenge_method", value: "S256"),
        ]
        guard let authURL = comps.url else { return false }

        print("Opening browser for Google Drive sign-in...")
        print("If the browser does not open, visit:\n\(authURL.absoluteString)\n")
        fflush(stdout)

        let openProc = Process()
        openProc.executableURL = URL(fileURLWithPath: "/usr/bin/open")
        openProc.arguments = [authURL.absoluteString]
        try? openProc.run()

        let clientSock = accept(listenSock, nil, nil)
        guard clientSock >= 0 else {
            fputs("Error: Failed to accept OAuth callback.\n", stderr)
            return false
        }
        defer { close(clientSock) }

        var buf = [UInt8](repeating: 0, count: 4096)
        let n = recv(clientSock, &buf, buf.count - 1, 0)
        var code = ""
        if n > 0, let reqStr = String(bytes: buf[0..<n], encoding: .utf8) {
            if let firstLine = reqStr.components(separatedBy: "\r\n").first {
                let parts = firstLine.components(separatedBy: " ")
                if parts.count >= 2, let urlComps = URLComponents(string: "http://127.0.0.1\(parts[1])") {
                    code = urlComps.queryItems?.first(where: { $0.name == "code" })?.value ?? ""
                }
            }
        }

        let html = """
        HTTP/1.1 200 OK\r
        Content-Type: text/html; charset=utf-8\r
        Connection: close\r
        \r
        <html><body style='font-family:sans-serif;text-align:center;margin-top:60px;'>
        <h2>&#10004; Google Drive Connected!</h2>
        <p>You can close this tab and return to Terminal.</p>
        </body></html>
        """
        _ = html.withCString { send(clientSock, $0, strlen($0), 0) }

        guard !code.isEmpty else {
            fputs("Error: Did not receive authorization code.\n", stderr)
            return false
        }

        var params = [
            "code": code,
            "client_id": auth.clientId,
            "redirect_uri": redirectUri,
            "grant_type": "authorization_code",
            "code_verifier": verifier,
        ]
        if !auth.clientSecret.isEmpty {
            params["client_secret"] = auth.clientSecret
        }

        guard let (status, json) = postForm(url: URL(string: "https://oauth2.googleapis.com/token")!, params: params),
              status == 200,
              let token = json["access_token"] as? String,
              let refresh = json["refresh_token"] as? String else {
            fputs("Error: Failed to exchange authorization code for tokens.\n", stderr)
            return false
        }

        let expiresIn = (json["expires_in"] as? Double) ?? 3600
        self.accessToken = token
        self.accessTokenExpiry = Date().addingTimeInterval(max(60, expiresIn - 60))
        self.auth.refreshToken = refresh

        do {
            try saveAuth()
            print("Successfully connected to Google Drive!")
            return true
        } catch {
            fputs("Error saving credentials: \(error.localizedDescription)\n", stderr)
            return false
        }
    }

    func ensureAccessToken() -> Bool {
        if !accessToken.isEmpty && Date() < accessTokenExpiry {
            return true
        }
        var params = [
            "client_id": auth.clientId,
            "refresh_token": auth.refreshToken,
            "grant_type": "refresh_token",
        ]
        if !auth.clientSecret.isEmpty {
            params["client_secret"] = auth.clientSecret
        }
        guard let (status, json) = postForm(url: URL(string: "https://oauth2.googleapis.com/token")!, params: params),
              status == 200,
              let token = json["access_token"] as? String else {
            return false
        }
        let expiresIn = (json["expires_in"] as? Double) ?? 3600
        self.accessToken = token
        self.accessTokenExpiry = Date().addingTimeInterval(max(60, expiresIn - 60))
        return true
    }

    @discardableResult
    func ensureDriveFolder(folderName: String) -> Bool {
        if !auth.driveFolderId.isEmpty {
            return true
        }
        guard ensureAccessToken() else { return false }

        let escapedName = folderName.replacingOccurrences(of: "'", with: "\\'")
        let query = "name='\(escapedName)' and mimeType='application/vnd.google-apps.folder' and trashed=false"
        var searchComps = URLComponents(string: "https://www.googleapis.com/drive/v3/files")!
        searchComps.queryItems = [
            URLQueryItem(name: "q", value: query),
            URLQueryItem(name: "fields", value: "files(id,name)"),
            URLQueryItem(name: "pageSize", value: "1"),
        ]
        if let searchURL = searchComps.url {
            var req = URLRequest(url: searchURL)
            req.httpMethod = "GET"
            req.timeoutInterval = 15
            req.setValue("Bearer \(accessToken)", forHTTPHeaderField: "Authorization")
            let sem = DispatchSemaphore(value: 0)
            var foundId: String?
            URLSession.shared.dataTask(with: req) { data, resp, _ in
                if let http = resp as? HTTPURLResponse, http.statusCode == 200,
                   let data = data,
                   let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
                   let files = json["files"] as? [[String: Any]],
                   let firstId = files.first?["id"] as? String, !firstId.isEmpty {
                    foundId = firstId
                }
                sem.signal()
            }.resume()
            sem.wait()

            if let id = foundId {
                auth.driveFolderId = id
                try? saveAuth()
                return true
            }
        }

        let createURL = URL(string: "https://www.googleapis.com/drive/v3/files")!
        var createReq = URLRequest(url: createURL)
        createReq.httpMethod = "POST"
        createReq.timeoutInterval = 15
        createReq.setValue("Bearer \(accessToken)", forHTTPHeaderField: "Authorization")
        createReq.setValue("application/json; charset=UTF-8", forHTTPHeaderField: "Content-Type")
        let createPayload: [String: Any] = [
            "name": folderName,
            "mimeType": "application/vnd.google-apps.folder",
        ]
        createReq.httpBody = try? JSONSerialization.data(withJSONObject: createPayload)

        let sem = DispatchSemaphore(value: 0)
        var createdId: String?
        URLSession.shared.dataTask(with: createReq) { data, resp, _ in
            if let http = resp as? HTTPURLResponse, (http.statusCode == 200 || http.statusCode == 201),
               let data = data,
               let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
               let newId = json["id"] as? String, !newId.isEmpty {
                createdId = newId
            }
            sem.signal()
        }.resume()
        sem.wait()

        if let id = createdId {
            auth.driveFolderId = id
            try? saveAuth()
            return true
        }
        return false
    }

    func upload(filename: String, mimeType: String, imageData: Data, folderName: String = defaultDriveFolderName) -> Bool {
        guard ensureAccessToken() else { return false }
        if auth.driveFolderId.isEmpty {
            ensureDriveFolder(folderName: folderName)
        }

        var metaObj: [String: Any] = ["name": filename]
        if !auth.driveFolderId.isEmpty {
            metaObj["parents"] = [auth.driveFolderId]
        }
        guard let metaData = try? JSONSerialization.data(withJSONObject: metaObj) else { return false }

        let boundary = "snap_drive_boundary_9f8e7d6c"
        var body = Data()
        body.append(Data("--\(boundary)\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n".utf8))
        body.append(metaData)
        body.append(Data("\r\n--\(boundary)\r\nContent-Type: \(mimeType)\r\n\r\n".utf8))
        body.append(imageData)
        body.append(Data("\r\n--\(boundary)--\r\n".utf8))

        let url = URL(string: "https://www.googleapis.com/upload/drive/v3/files?uploadType=multipart")!
        let status = sendUpload(url: url, boundary: boundary, body: body)
        if status == 401 {
            accessToken = ""
            if ensureAccessToken() {
                let retryStatus = sendUpload(url: url, boundary: boundary, body: body)
                return retryStatus == 200 || retryStatus == 201
            }
            return false
        }
        return status == 200 || status == 201
    }

    private func sendUpload(url: URL, boundary: String, body: Data) -> Int {
        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.timeoutInterval = 30
        req.setValue("Bearer \(accessToken)", forHTTPHeaderField: "Authorization")
        req.setValue("multipart/related; boundary=\(boundary)", forHTTPHeaderField: "Content-Type")
        req.httpBody = body

        let sem = DispatchSemaphore(value: 0)
        var statusCode = 0
        URLSession.shared.dataTask(with: req) { _, resp, _ in
            if let http = resp as? HTTPURLResponse {
                statusCode = http.statusCode
            }
            sem.signal()
        }.resume()
        sem.wait()
        return statusCode
    }

    private func postForm(url: URL, params: [String: String]) -> (Int, [String: Any])? {
        var comps = URLComponents()
        comps.queryItems = params.map { URLQueryItem(name: $0.key, value: $0.value) }
        let bodyString = comps.percentEncodedQuery ?? ""

        var req = URLRequest(url: url)
        req.httpMethod = "POST"
        req.timeoutInterval = 20
        req.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        req.httpBody = Data(bodyString.utf8)

        let sem = DispatchSemaphore(value: 0)
        var result: (Int, [String: Any])?
        URLSession.shared.dataTask(with: req) { data, resp, _ in
            if let http = resp as? HTTPURLResponse,
               let data = data,
               let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                result = (http.statusCode, json)
            }
            sem.signal()
        }.resume()
        sem.wait()
        return result
    }

    private func base64UrlEncode(_ data: Data) -> String {
        data.base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }
}

// MARK: - Filename & Upload Worker

private let nameLock = NSLock()
private var lastBaseTimestamp = ""
private var sameSecondSeq = 0

func timestampFilename(format: String, checkDir: URL? = nil) -> String {
    nameLock.lock()
    defer { nameLock.unlock() }

    let df = DateFormatter()
    df.locale = Locale(identifier: "en_US_POSIX")
    df.dateFormat = "yyyy-MM-dd_HH-mm-ss"
    let ts = df.string(from: Date())
    if ts == lastBaseTimestamp {
        sameSecondSeq += 1
    } else {
        lastBaseTimestamp = ts
        sameSecondSeq = 0
    }
    let base = (sameSecondSeq == 0) ? "screen_\(ts)" : "screen_\(ts)_\(sameSecondSeq)"
    var name = "\(base).\(format)"
    if let dir = checkDir {
        var counter = 1
        while FileManager.default.fileExists(atPath: dir.appendingPathComponent(name).path) {
            name = "\(base)_\(counter).\(format)"
            counter += 1
        }
    }
    return name
}

func flushPendingQueue(queueDir: URL, drive: DriveClient, folderName: String) {
    guard let items = try? FileManager.default.contentsOfDirectory(
        at: queueDir,
        includingPropertiesForKeys: nil,
        options: [.skipsHiddenFiles]
    ) else { return }

    let sorted = items.sorted { $0.lastPathComponent < $1.lastPathComponent }
    for fileURL in sorted {
        guard let data = try? Data(contentsOf: fileURL) else { continue }
        let ext = fileURL.pathExtension.lowercased()
        let mime = (ext == "png") ? "image/png" : "image/jpeg"
        if drive.upload(filename: fileURL.lastPathComponent, mimeType: mime, imageData: data, folderName: folderName) {
            try? FileManager.default.removeItem(at: fileURL)
            print("[Queue] Uploaded & deleted: \(fileURL.lastPathComponent)")
            fflush(stdout)
        } else {
            break
        }
    }
}

final class UploadWorker {
    private let drive: DriveClient
    private let queueDir: URL
    private let folderName: String
    private let lock = NSLock()
    private let sem = DispatchSemaphore(value: 0)
    private var memoryQueue: [(name: String, mime: String, data: Data)] = []
    private var stopRequested = false
    private let doneSem = DispatchSemaphore(value: 0)
    private(set) var uploadedCount = 0

    init(drive: DriveClient, queueDir: URL, folderName: String) {
        self.drive = drive
        self.queueDir = queueDir
        self.folderName = folderName
        DispatchQueue.global(qos: .utility).async { [weak self] in
            self?.runLoop()
        }
    }

    func enqueue(name: String, mime: String, data: Data) {
        lock.lock()
        if memoryQueue.count >= 30 {
            try? FileManager.default.createDirectory(at: queueDir, withIntermediateDirectories: true)
            let qURL = queueDir.appendingPathComponent(name)
            try? data.write(to: qURL, options: .atomic)
            lock.unlock()
            print("[Offline/Full] Queued to disk: \(name)")
            fflush(stdout)
            sem.signal()
            return
        }
        memoryQueue.append((name, mime, data))
        lock.unlock()
        sem.signal()
    }

    func stopAndDrain(timeoutSeconds: Double = 10.0) {
        lock.lock()
        stopRequested = true
        lock.unlock()
        sem.signal()
        _ = doneSem.wait(timeout: .now() + timeoutSeconds)
    }

    private func runLoop() {
        defer { doneSem.signal() }
        while true {
            flushPendingQueue(queueDir: queueDir, drive: drive, folderName: folderName)

            lock.lock()
            let item = memoryQueue.isEmpty ? nil : memoryQueue.removeFirst()
            let shouldStop = stopRequested
            lock.unlock()

            if let item = item {
                if drive.upload(filename: item.name, mimeType: item.mime, imageData: item.data, folderName: folderName) {
                    lock.lock()
                    uploadedCount += 1
                    let count = uploadedCount
                    lock.unlock()
                    print("[\(count)] Uploaded to Drive: \(item.name) (\(item.data.count / 1024) KB)")
                    fflush(stdout)
                } else {
                    try? FileManager.default.createDirectory(at: queueDir, withIntermediateDirectories: true)
                    let qURL = queueDir.appendingPathComponent(item.name)
                    if (try? item.data.write(to: qURL, options: .atomic)) != nil {
                        print("[Offline] Queued temporarily: \(item.name)")
                        fflush(stdout)
                    }
                }
                continue
            }

            if shouldStop {
                break
            }
            _ = sem.wait(timeout: .now() + 2.0)
        }
    }
}

// MARK: - Event-Driven SCStream Capture (macOS)

let sampleGridW = 640
let sampleGridH = 360

func samplePixelBuffer(_ pixelBuffer: CVPixelBuffer) -> [UInt8]? {
    CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
    defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }

    guard let base = CVPixelBufferGetBaseAddress(pixelBuffer) else { return nil }
    let w = CVPixelBufferGetWidth(pixelBuffer)
    let h = CVPixelBufferGetHeight(pixelBuffer)
    let bpr = CVPixelBufferGetBytesPerRow(pixelBuffer)
    guard w > 0, h > 0 else { return nil }

    let ptr = base.assumingMemoryBound(to: UInt8.self)
    var out = [UInt8](repeating: 0, count: sampleGridW * sampleGridH * 3)
    var idx = 0
    for gy in 0..<sampleGridH {
        let sy = (gy * h) / sampleGridH
        let row = ptr + (sy * bpr)
        for gx in 0..<sampleGridW {
            let sx = (gx * w) / sampleGridW
            let px = row + (sx * 4)
            out[idx] = px[0]
            out[idx + 1] = px[1]
            out[idx + 2] = px[2]
            idx += 3
        }
    }
    return out
}

func computeSampleDiffPercent(prev: [UInt8], curr: [UInt8]) -> Double {
    guard !prev.isEmpty, prev.count == curr.count else { return 100.0 }
    let total = prev.count / 3
    var changed = 0
    let threshold = 15
    var i = 0
    while i < prev.count {
        let db = abs(Int(prev[i]) - Int(curr[i]))
        let dg = abs(Int(prev[i + 1]) - Int(curr[i + 1]))
        let dr = abs(Int(prev[i + 2]) - Int(curr[i + 2]))
        if db > threshold || dg > threshold || dr > threshold {
            changed += 1
        }
        i += 3
    }
    return (Double(changed) * 100.0) / Double(total)
}

func encodePixelBuffer(_ pixelBuffer: CVPixelBuffer, format: String) -> Data? {
    CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
    defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }

    guard let base = CVPixelBufferGetBaseAddress(pixelBuffer) else { return nil }
    let w = CVPixelBufferGetWidth(pixelBuffer)
    let h = CVPixelBufferGetHeight(pixelBuffer)
    let bpr = CVPixelBufferGetBytesPerRow(pixelBuffer)
    let totalBytes = bpr * h

    let colorSpace = CGColorSpaceCreateDeviceRGB()
    let bitmapInfo = CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.premultipliedFirst.rawValue
    guard let provider = CGDataProvider(dataInfo: nil, data: base, size: totalBytes, releaseData: { _, _, _ in }),
          let cgImg = CGImage(
              width: w,
              height: h,
              bitsPerComponent: 8,
              bitsPerPixel: 32,
              bytesPerRow: bpr,
              space: colorSpace,
              bitmapInfo: CGBitmapInfo(rawValue: bitmapInfo),
              provider: provider,
              decode: nil,
              shouldInterpolate: false,
              intent: .defaultIntent
          ) else {
        return nil
    }

    return encodeCGImage(cgImg, format: format)
}

func encodeCGImage(_ cgImg: CGImage, format: String) -> Data? {
    let utType = (format == "png") ? UTType.png.identifier as CFString : UTType.jpeg.identifier as CFString
    let outData = NSMutableData()
    guard let dest = CGImageDestinationCreateWithData(outData, utType, 1, nil) else { return nil }
    let props: [CFString: Any] = (format == "png") ? [:] : [kCGImageDestinationLossyCompressionQuality: 0.80]
    CGImageDestinationAddImage(dest, cgImg, props as CFDictionary)
    guard CGImageDestinationFinalize(dest) else { return nil }
    return outData as Data
}

final class EventStreamCapturer: NSObject, SCStreamOutput, SCStreamDelegate {
    private let filter: SCContentFilter
    private let displayWidth: Int
    private let displayHeight: Int
    private let format: String
    private let diffThresholdPercent: Double
    private let onImage: (Data) -> Void

    private let stateLock = NSLock()
    private var prevSample: [UInt8] = []
    private var stream: SCStream?
    private let streamQueue = DispatchQueue(label: "snap.scstream.output", qos: .userInitiated)

    init?(displayNumber: Int?, format: String, diffThresholdPercent: Double, onImage: @escaping (Data) -> Void) {
        let sem = DispatchSemaphore(value: 0)
        var pickedDisplay: SCDisplay?
        SCShareableContent.getExcludingDesktopWindows(false, onScreenWindowsOnly: true) { content, _ in
            if let displays = content?.displays, !displays.isEmpty {
                let idx = max(0, (displayNumber ?? 1) - 1)
                if idx < displays.count {
                    pickedDisplay = displays[idx]
                }
            }
            sem.signal()
        }
        sem.wait()

        guard let display = pickedDisplay else { return nil }
        self.filter = SCContentFilter(display: display, excludingWindows: [])
        self.displayWidth = display.width
        self.displayHeight = display.height
        self.format = format
        self.diffThresholdPercent = diffThresholdPercent
        self.onImage = onImage
        super.init()
    }

    func captureSingleShot() -> Data? {
        let cfg = SCStreamConfiguration()
        cfg.width = displayWidth
        cfg.height = displayHeight
        cfg.showsCursor = false

        let sem = DispatchSemaphore(value: 0)
        var cgImg: CGImage?
        SCScreenshotManager.captureImage(contentFilter: filter, configuration: cfg) { img, _ in
            cgImg = img
            sem.signal()
        }
        sem.wait()

        guard let img = cgImg else { return nil }
        return encodeCGImage(img, format: format)
    }

    func start() -> Bool {
        let cfg = SCStreamConfiguration()
        cfg.width = displayWidth
        cfg.height = displayHeight
        cfg.showsCursor = false
        cfg.pixelFormat = kCVPixelFormatType_32BGRA
        cfg.minimumFrameInterval = CMTime(value: 1, timescale: 10)  // up to 10 OS events/sec
        cfg.queueDepth = 3

        let scStream = SCStream(filter: filter, configuration: cfg, delegate: self)
        do {
            try scStream.addStreamOutput(self, type: .screen, sampleHandlerQueue: streamQueue)
        } catch {
            fputs("Error adding SCStream output: \(error.localizedDescription)\n", stderr)
            return false
        }

        let sem = DispatchSemaphore(value: 0)
        var started = false
        scStream.startCapture { err in
            started = (err == nil)
            if let err = err {
                fputs("Error starting SCStream: \(err.localizedDescription)\n", stderr)
            }
            sem.signal()
        }
        sem.wait()
        self.stream = scStream
        return started
    }

    func stop() {
        guard let scStream = stream else { return }
        let sem = DispatchSemaphore(value: 0)
        scStream.stopCapture { _ in sem.signal() }
        _ = sem.wait(timeout: .now() + 3.0)
        stream = nil
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, sampleBuffer.isValid else { return }

        guard let attachmentsArray = CMSampleBufferGetSampleAttachmentsArray(
            sampleBuffer,
            createIfNecessary: false
        ) as? [[SCStreamFrameInfo: Any]],
              let attachments = attachmentsArray.first,
              let statusRaw = attachments[.status] as? Int,
              let status = SCFrameStatus(rawValue: statusRaw),
              status == .complete else {
            return
        }

        if let dirtyRects = attachments[.dirtyRects] as? [CGRect], dirtyRects.isEmpty {
            return
        }

        guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer),
              let currSample = samplePixelBuffer(pixelBuffer) else {
            return
        }

        stateLock.lock()
        let isFirst = prevSample.isEmpty
        let diff = isFirst ? 100.0 : computeSampleDiffPercent(prev: prevSample, curr: currSample)
        let shouldCapture = isFirst || (diffThresholdPercent == 0.0) || (diff >= diffThresholdPercent)
        if shouldCapture {
            prevSample = currSample
        }
        stateLock.unlock()

        if shouldCapture, let data = encodePixelBuffer(pixelBuffer, format: format) {
            onImage(data)
        }
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        fputs("SCStream stopped with error: \(error.localizedDescription)\n", stderr)
    }
}

// MARK: - Terminal Raw Input Helpers

private var originalTermios = termios()
private var termiosSaved = false

func enableRawMode() {
    guard isatty(STDIN_FILENO) != 0 else { return }
    if tcgetattr(STDIN_FILENO, &originalTermios) == 0 {
        termiosSaved = true
        var raw = originalTermios
        raw.c_lflag &= ~UInt(ICANON | ECHO)
        withUnsafeMutablePointer(to: &raw.c_cc) { ptr in
            let buf = UnsafeMutableRawPointer(ptr).assumingMemoryBound(to: cc_t.self)
            buf[Int(VMIN)] = 0
            buf[Int(VTIME)] = 1
        }
        tcsetattr(STDIN_FILENO, TCSANOW, &raw)
    }
}

func restoreTermMode() {
    if termiosSaved {
        tcsetattr(STDIN_FILENO, TCSANOW, &originalTermios)
        termiosSaved = false
    }
}

func readKeyNonBlocking() -> UInt8? {
    var ch: UInt8 = 0
    let n = read(STDIN_FILENO, &ch, 1)
    return n == 1 ? ch : nil
}

// MARK: - Main

func main() -> Int32 {
    guard let opts = parseArgs(CommandLine.arguments) else {
        return 1
    }
    if opts.showHelp {
        printUsage()
        return 0
    }

    let cwdURL = URL(fileURLWithPath: FileManager.default.currentDirectoryPath, isDirectory: true)
    let authFileURL = cwdURL.appendingPathComponent("snap_auth.json")
    let queueDirURL = cwdURL.appendingPathComponent("pending_upload", isDirectory: true)
    let credURL = cwdURL.appendingPathComponent("credentials.json")

    if opts.doLogout {
        try? FileManager.default.removeItem(at: authFileURL)
        print("Logged out from Google Drive (removed snap_auth.json).")
        return 0
    }

    let (cid, csec) = DriveClient.loadCredentialsJsonIfPresent(at: credURL) ?? (defaultClientId, defaultClientSecret)
    var driveClient: DriveClient? = nil

    if opts.doLogin {
        let client = DriveClient(
            auth: AuthData(clientId: cid, clientSecret: csec, refreshToken: "", driveFolderId: opts.driveFolderId),
            authFileURL: authFileURL
        )
        return client.performOAuthLogin() ? 0 : 1
    }

    if !opts.localOnly {
        if let saved = DriveClient.loadAuth(from: authFileURL) {
            var updated = saved
            if !opts.driveFolderId.isEmpty { updated.driveFolderId = opts.driveFolderId }
            driveClient = DriveClient(auth: updated, authFileURL: authFileURL)
        } else {
            print("First launch: Google Drive sign-in is required.")
            let client = DriveClient(
                auth: AuthData(clientId: cid, clientSecret: csec, refreshToken: "", driveFolderId: opts.driveFolderId),
                authFileURL: authFileURL
            )
            guard client.performOAuthLogin() else {
                return 1
            }
            driveClient = client
        }
    } else {
        try? FileManager.default.createDirectory(
            at: URL(fileURLWithPath: opts.outputDir, isDirectory: true),
            withIntermediateDirectories: true
        )
    }

    var localShotCount = 0
    let localLock = NSLock()
    let mime = (opts.format == "png") ? "image/png" : "image/jpeg"
    let worker = (!opts.once && driveClient != nil)
        ? UploadWorker(drive: driveClient!, queueDir: queueDirURL, folderName: opts.driveFolderName)
        : nil

    let handleImage: (Data) -> Void = { imgBytes in
        if opts.localOnly {
            let outDir = URL(fileURLWithPath: opts.outputDir, isDirectory: true)
            let name = timestampFilename(format: opts.format, checkDir: outDir)
            let outURL = outDir.appendingPathComponent(name)
            do {
                try imgBytes.write(to: outURL, options: .atomic)
                localLock.lock()
                localShotCount += 1
                let c = localShotCount
                localLock.unlock()
                print("[\(c)] Saved locally: \(outURL.path)")
                fflush(stdout)
            } catch {
                fputs("Save failed: \(error.localizedDescription)\n", stderr)
            }
        } else if let worker = worker {
            let name = timestampFilename(format: opts.format)
            worker.enqueue(name: name, mime: mime, data: imgBytes)
        } else if let drive = driveClient {
            let name = timestampFilename(format: opts.format)
            if drive.upload(filename: name, mimeType: mime, imageData: imgBytes, folderName: opts.driveFolderName) {
                localLock.lock()
                localShotCount += 1
                let c = localShotCount
                localLock.unlock()
                print("[\(c)] Uploaded to Drive: \(name) (\(imgBytes.count / 1024) KB)")
                fflush(stdout)
            } else {
                try? FileManager.default.createDirectory(at: queueDirURL, withIntermediateDirectories: true)
                let qName = timestampFilename(format: opts.format, checkDir: queueDirURL)
                let qURL = queueDirURL.appendingPathComponent(qName)
                if (try? imgBytes.write(to: qURL, options: .atomic)) != nil {
                    print("[Offline] Queued temporarily: \(qName)")
                    fflush(stdout)
                }
            }
        }
    }

    guard let capturer = EventStreamCapturer(
        displayNumber: opts.displayNumber,
        format: opts.format,
        diffThresholdPercent: opts.diffThresholdPercent,
        onImage: handleImage
    ) else {
        fputs("Error: Could not initialize ScreenCaptureKit for display.\n", stderr)
        return 1
    }

    if opts.once {
        if let data = capturer.captureSingleShot() {
            handleImage(data)
            return 0
        }
        fputs("Capture failed.\n", stderr)
        return 1
    }

    guard capturer.start() else {
        return 1
    }

    enableRawMode()
    defer {
        restoreTermMode()
        capturer.stop()
    }

    signal(SIGINT) { _ in
        restoreTermMode()
        print("\nStopped.")
        exit(0)
    }

    let dest = opts.localOnly ? "local (\(opts.outputDir)/)" : "Google Drive"
    print("OS Event-Driven capture active (SCStream, diff >= \(opts.diffThresholdPercent)%, \(opts.format)) -> \(dest)")
    print("Waiting for macOS screen-change events... Press SPACE/ENTER for manual shot, 'q' or Ctrl+C to stop.")

    while true {
        if isatty(STDIN_FILENO) != 0 {
            if let key = readKeyNonBlocking() {
                if key == UInt8(ascii: "q") || key == UInt8(ascii: "Q") {
                    break
                } else if key == UInt8(ascii: " ") || key == UInt8(ascii: "\n") || key == UInt8(ascii: "\r") {
                    if let data = capturer.captureSingleShot() {
                        handleImage(data)
                    }
                }
            }
        }
        Thread.sleep(forTimeInterval: 0.1)
    }

    capturer.stop()
    worker?.stopAndDrain()
    let finalCount = worker?.uploadedCount ?? localShotCount
    print("Done. Uploaded/captured \(finalCount) screenshot(s).")
    return 0
}

exit(main())
