#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <objidl.h>
#include <objbase.h>
#include <gdiplus.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <shellapi.h>
#include <conio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

#ifndef SNAP_VERSION
#define SNAP_VERSION "1.3.0"
#endif

// Override at compile time via -DDEFAULT_CLIENT_ID="..." -DDEFAULT_CLIENT_SECRET="..."
// or place credentials.json next to snap.exe
#ifndef DEFAULT_CLIENT_ID
#define DEFAULT_CLIENT_ID ""
#endif
#ifndef DEFAULT_CLIENT_SECRET
#define DEFAULT_CLIENT_SECRET ""
#endif

static const wchar_t* REG_RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* REG_VALUE_NAME = L"SnapLectureCapture";
static const char* DRIVE_SCOPE = "https://www.googleapis.com/auth/drive";
static const char* DEFAULT_DRIVE_FOLDER_NAME = "screenshot";

struct Options {
    int pollMs = 100;                    // DXGI AcquireNextFrame wait timeout (ms)
    double intervalSeconds = 0.0;        // 0 = pure OS event-driven
    double diffThresholdPercent = 0.02;  // 0 = all OS events
    int jpegQuality = 65;
    bool once = false;
    int displayNumber = 0;  // 0 = primary display, >= 1 = specific display
    std::string format = "jpg";
    std::string outputDir;  // used only with --local
    bool localOnly = false;
    std::string driveFolderId;
    std::string driveFolderName = DEFAULT_DRIVE_FOLDER_NAME;
    bool background = false;
    bool installStartup = false;
    bool removeStartup = false;
    bool startupStatus = false;
    bool doLogin = false;
    bool doLogout = false;
    bool cleanDrive = false;
    bool assumeYes = false;
    std::string clientId;
    std::string clientSecret;
    bool setup = false;
    bool showHelp = false;
    bool showVersion = false;
};

struct AuthData {
    std::string clientId;
    std::string clientSecret;
    std::string refreshToken;
    std::string driveFolderId;
    std::string accessToken;
    std::chrono::steady_clock::time_point accessTokenExpiry{};
};

static volatile bool g_running = true;

BOOL WINAPI ConsoleHandler(DWORD ctrlType) {
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

fs::path getExePath() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0) {
        return fs::current_path() / "snap.exe";
    }
    return fs::path(std::wstring(buf, len));
}

static std::mutex g_logMtx;
static fs::path g_logPath;

void logMsg(const std::string& msg) {
    if (g_logPath.empty()) return;
    std::lock_guard<std::mutex> lock(g_logMtx);
    std::error_code ec;
    if (fs::exists(g_logPath, ec) && fs::file_size(g_logPath, ec) > 512 * 1024) {
        fs::remove(g_logPath, ec);
    }
    std::time_t now = std::time(nullptr);
    std::tm tmNow{};
    localtime_s(&tmNow, &now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmNow);
    std::ofstream f(g_logPath, std::ios::app);
    if (f) {
        f << "[" << buf << "] " << msg << "\n";
    }
}

void printUsage() {
    std::cout <<
        "snap.exe - Continuous screen capture to Google Drive (OAuth) for lectures\n\n"
        "USAGE:\n"
        "  snap.exe [OPTIONS]\n\n"
        "DEFAULT:\n"
        "  Captures 1 initial shot, then watches the screen every 100ms (lightweight)\n"
        "  and captures/uploads ONLY when movement/change is detected (>= 0.01%).\n"
        "  If offline, saves temporarily to <exe_dir>\\pending_upload\\ and deletes after upload.\n"
        "  Press SPACE or ENTER for an immediate shot, 'q' or Ctrl+C to stop.\n\n"
        "GOOGLE DRIVE / OAUTH:\n"
        "  --login                Sign in with Google OAuth (opens browser once)\n"
        "  --logout               Remove saved Google OAuth credentials\n"
        "  --client-id ID         Google OAuth Desktop Client ID (used with --login)\n"
        "  --client-secret SEC    Google OAuth Desktop Client Secret (if applicable)\n"
        "  --folder ID            Google Drive folder ID to upload into (optional)\n"
        "  --folder-name NAME     Auto-created Drive folder name (default: screenshot)\n"
        "  --clean-drive          Permanently delete everything in Google Drive & Trash\n"
        "                         EXCEPT images inside the app's folder\n"
        "  -y, --yes              Skip confirmation prompt for --clean-drive\n"
        "  --local                Save locally to disk instead of uploading to Drive\n\n"
        "CAPTURE OPTIONS:\n"
        "  --poll-ms MS           Motion check interval in milliseconds (default: 100)\n"
        "  -i, --interval SEC     Optional min seconds between captures (default: 0 = instant)\n"
        "  --diff PERCENT         Min screen change %% to upload (default: 0.01, 0 = always)\n"
        "  -d, --display NUM      Display number (1 = first, 2 = second, ...)\n"
        "  -f, --format FMT       Image format: jpg or png (default: jpg)\n"
        "  -q, --quality NUM      JPEG quality 10-100 (default: 80)\n"
        "  -o, --out DIR          Local output folder (with --local)\n"
        "  -1, --once             Capture/upload a single screenshot and exit\n"
        "  -b, --background       Hide console window and run silently in background\n\n"
        "STARTUP & CONFIG:\n"
        "  --install-startup      Enable auto-run when Windows starts\n"
        "  --remove-startup       Disable auto-run on Windows startup\n"
        "  --startup-status       Check if auto-run on startup is enabled\n"
        "  --setup                Interactive setup wizard\n"
        "  -h, --help             Show this help message\n";
}

bool parseArgs(int argc, char* argv[], Options& opts) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            opts.showHelp = true;
            return true;
        } else if (arg == "-v" || arg == "--version") {
            opts.showVersion = true;
            return true;
        } else if (arg == "-1" || arg == "--once") {
            opts.once = true;
        } else if (arg == "-b" || arg == "--background") {
            opts.background = true;
        } else if (arg == "--local") {
            opts.localOnly = true;
        } else if (arg == "--install-startup") {
            opts.installStartup = true;
        } else if (arg == "--remove-startup") {
            opts.removeStartup = true;
        } else if (arg == "--startup-status") {
            opts.startupStatus = true;
        } else if (arg == "--login") {
            opts.doLogin = true;
        } else if (arg == "--logout") {
            opts.doLogout = true;
        } else if (arg == "--clean-drive") {
            opts.cleanDrive = true;
        } else if (arg == "-y" || arg == "--yes") {
            opts.assumeYes = true;
        } else if (arg == "--setup") {
            opts.setup = true;
        } else if (arg == "--client-id") {
            if (i + 1 >= argc) { std::cerr << "Error: --client-id requires a value.\n"; return false; }
            opts.clientId = argv[++i];
        } else if (arg == "--client-secret") {
            if (i + 1 >= argc) { std::cerr << "Error: --client-secret requires a value.\n"; return false; }
            opts.clientSecret = argv[++i];
        } else if (arg == "--folder") {
            if (i + 1 >= argc) { std::cerr << "Error: --folder requires a Drive folder ID.\n"; return false; }
            opts.driveFolderId = argv[++i];
        } else if (arg == "--folder-name") {
            if (i + 1 >= argc || std::strlen(argv[i + 1]) == 0) { std::cerr << "Error: --folder-name requires a name.\n"; return false; }
            opts.driveFolderName = argv[++i];
        } else if (arg == "--poll-ms") {
            if (i + 1 >= argc) { std::cerr << "Error: --poll-ms requires milliseconds.\n"; return false; }
            try {
                int val = std::stoi(argv[++i]);
                if (val < 20) throw std::invalid_argument("min 20ms");
                opts.pollMs = val;
            } catch (...) {
                std::cerr << "Error: --poll-ms requires >= 20.\n";
                return false;
            }
        } else if (arg == "-i" || arg == "--interval") {
            if (i + 1 >= argc) { std::cerr << "Error: --interval requires seconds.\n"; return false; }
            try {
                double val = std::stod(argv[++i]);
                if (val < 0) throw std::invalid_argument("negative");
                opts.intervalSeconds = val;
            } catch (...) {
                std::cerr << "Error: --interval requires >= 0.\n";
                return false;
            }
        } else if (arg == "--diff") {
            if (i + 1 >= argc) { std::cerr << "Error: --diff requires a percentage.\n"; return false; }
            try {
                double val = std::stod(argv[++i]);
                if (val < 0 || val > 100) throw std::invalid_argument("range");
                opts.diffThresholdPercent = val;
            } catch (...) {
                std::cerr << "Error: --diff must be between 0 and 100.\n";
                return false;
            }
        } else if (arg == "-q" || arg == "--quality") {
            if (i + 1 >= argc) { std::cerr << "Error: --quality requires 10-100.\n"; return false; }
            try {
                int val = std::stoi(argv[++i]);
                if (val < 10 || val > 100) throw std::invalid_argument("range");
                opts.jpegQuality = val;
            } catch (...) {
                std::cerr << "Error: --quality must be between 10 and 100.\n";
                return false;
            }
        } else if (arg == "-d" || arg == "--display") {
            if (i + 1 >= argc) { std::cerr << "Error: --display requires >= 1.\n"; return false; }
            try {
                int val = std::stoi(argv[++i]);
                if (val < 1) throw std::invalid_argument("invalid");
                opts.displayNumber = val;
            } catch (...) {
                std::cerr << "Error: --display requires >= 1.\n";
                return false;
            }
        } else if (arg == "-f" || arg == "--format") {
            if (i + 1 >= argc) { std::cerr << "Error: --format requires 'jpg' or 'png'.\n"; return false; }
            std::string fmt = argv[++i];
            for (char& c : fmt) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (fmt != "jpg" && fmt != "png") {
                std::cerr << "Error: --format must be 'jpg' or 'png'.\n";
                return false;
            }
            opts.format = fmt;
        } else if (arg == "-o" || arg == "--out") {
            if (i + 1 >= argc || std::strlen(argv[i + 1]) == 0) {
                std::cerr << "Error: --out requires a directory path.\n";
                return false;
            }
            opts.outputDir = argv[++i];
        } else {
            std::cerr << "Error: Unknown option '" << arg << "'. Use --help for usage.\n";
            return false;
        }
    }
    return true;
}

// MARK: - String & Encoding Utilities

std::wstring toWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring ws(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

std::string urlEncode(const std::string& s) {
    std::ostringstream out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else {
            out << '%' << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(c);
        }
    }
    return out.str();
}

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hexVal = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                return -1;
            };
            int hi = hexVal(s[i + 1]);
            int lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        } else if (s[i] == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string base64UrlEncode(const std::vector<BYTE>& data) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    size_t i = 0;
    while (i < data.size()) {
        size_t remBytes = data.size() - i;
        uint32_t a = data[i++];
        uint32_t b = (remBytes > 1) ? data[i++] : 0;
        uint32_t c = (remBytes > 2) ? data[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out.push_back(tbl[(triple >> 18) & 0x3F]);
        out.push_back(tbl[(triple >> 12) & 0x3F]);
        if (remBytes > 1) out.push_back(tbl[(triple >> 6) & 0x3F]);
        if (remBytes > 2) out.push_back(tbl[triple & 0x3F]);
    }
    return out;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string jsonExtractString(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    ++pos;
    std::string val;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '\\' && pos < json.size()) {
            char next = json[pos++];
            if (next == 'n') val += '\n';
            else if (next == 'r') val += '\r';
            else if (next == 't') val += '\t';
            else val += next;
        } else if (c == '"') {
            break;
        } else {
            val += c;
        }
    }
    return val;
}

long long jsonExtractInt(const std::string& json, const std::string& key, long long defaultVal) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return defaultVal;
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) return defaultVal;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    try {
        return std::stoll(json.substr(pos));
    } catch (...) {
        return defaultVal;
    }
}

// MARK: - DPAPI Encrypted Auth Storage

bool dpapiProtect(const std::string& plain, std::vector<BYTE>& outEncrypted) {
    DATA_BLOB inBlob{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB outBlob{};
    if (!CryptProtectData(&inBlob, L"SnapAuth", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &outBlob)) {
        return false;
    }
    outEncrypted.assign(outBlob.pbData, outBlob.pbData + outBlob.cbData);
    LocalFree(outBlob.pbData);
    return true;
}

bool dpapiUnprotect(const std::vector<BYTE>& encrypted, std::string& outPlain) {
    DATA_BLOB inBlob{static_cast<DWORD>(encrypted.size()), const_cast<BYTE*>(encrypted.data())};
    DATA_BLOB outBlob{};
    if (!CryptUnprotectData(&inBlob, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &outBlob)) {
        return false;
    }
    outPlain.assign(reinterpret_cast<char*>(outBlob.pbData), outBlob.cbData);
    LocalFree(outBlob.pbData);
    return true;
}

bool loadCredentialsJsonIfPresent(const fs::path& credPath, AuthData& auth) {
    std::ifstream f(credPath);
    if (!f) return false;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string cid = jsonExtractString(content, "client_id");
    std::string csec = jsonExtractString(content, "client_secret");
    if (cid.empty()) return false;
    auth.clientId = cid;
    if (!csec.empty()) auth.clientSecret = csec;
    return true;
}

bool saveAuthData(const fs::path& authFile, const AuthData& auth) {
    std::ostringstream ss;
    ss << auth.clientId << "\n"
       << auth.clientSecret << "\n"
       << auth.refreshToken << "\n"
       << auth.driveFolderId << "\n";
    std::vector<BYTE> enc;
    if (!dpapiProtect(ss.str(), enc)) return false;
    std::ofstream f(authFile, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(enc.data()), static_cast<std::streamsize>(enc.size()));
    return f.good();
}

bool loadAuthData(const fs::path& authFile, AuthData& auth) {
    std::ifstream f(authFile, std::ios::binary);
    if (!f) return false;
    std::vector<BYTE> enc((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (enc.empty()) return false;
    std::string plain;
    if (!dpapiUnprotect(enc, plain)) return false;

    std::istringstream ss(plain);
    std::getline(ss, auth.clientId);
    std::getline(ss, auth.clientSecret);
    std::getline(ss, auth.refreshToken);
    std::getline(ss, auth.driveFolderId);
    return !auth.clientId.empty() && !auth.refreshToken.empty();
}

// MARK: - WinHTTP Helper

struct HttpResponse {
    DWORD statusCode = 0;
    std::string body;
    bool networkOk = false;
};

HINTERNET getSharedWinHttpSession() {
    static std::mutex sessionMtx;
    static HINTERNET hSession = nullptr;
    std::lock_guard<std::mutex> lock(sessionMtx);
    if (!hSession) {
        hSession = WinHttpOpen(L"SnapLectureCapture/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (hSession) {
            WinHttpSetTimeouts(hSession, 8000, 8000, 20000, 20000);
#ifdef WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL
            DWORD http2 = WINHTTP_PROTOCOL_FLAG_HTTP2;
            WinHttpSetOption(hSession, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &http2, sizeof(http2));
#endif
        }
    }
    return hSession;
}

HttpResponse httpRequest(
    const std::wstring& method,
    const std::wstring& host,
    const std::wstring& path,
    const std::wstring& headers,
    const void* bodyData,
    DWORD bodySize
) {
    HttpResponse resp;
    HINTERNET hSession = getSharedWinHttpSession();
    if (!hSession) return resp;

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        return resp;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method.c_str(), path.c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        return resp;
    }

    BOOL sent = WinHttpSendRequest(
        hRequest,
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
        const_cast<void*>(bodyData),
        bodySize,
        bodySize,
        0
    );

    if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
        resp.networkOk = true;
        DWORD statusSize = sizeof(resp.statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &resp.statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

        DWORD bytesAvail = 0;
        while (WinHttpQueryDataAvailable(hRequest, &bytesAvail) && bytesAvail > 0) {
            std::string chunk(bytesAvail, '\0');
            DWORD bytesRead = 0;
            if (WinHttpReadData(hRequest, &chunk[0], bytesAvail, &bytesRead) && bytesRead > 0) {
                chunk.resize(bytesRead);
                resp.body += chunk;
            } else {
                break;
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    return resp;
}

HttpResponse httpPost(
    const std::wstring& host,
    const std::wstring& path,
    const std::wstring& headers,
    const void* bodyData,
    DWORD bodySize
) {
    return httpRequest(L"POST", host, path, headers, bodyData, bodySize);
}

HttpResponse httpGet(
    const std::wstring& host,
    const std::wstring& path,
    const std::wstring& headers
) {
    return httpRequest(L"GET", host, path, headers, nullptr, 0);
}

HttpResponse httpDelete(
    const std::wstring& host,
    const std::wstring& path,
    const std::wstring& headers
) {
    return httpRequest(L"DELETE", host, path, headers, nullptr, 0);
}

// MARK: - OAuth 2.0 PKCE Login & Token Refresh

bool generatePkce(std::string& verifier, std::string& challenge) {
    HCRYPTPROV hProv = 0;
    if (!CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        return false;
    }
    std::vector<BYTE> rnd(32);
    BOOL ok = CryptGenRandom(hProv, static_cast<DWORD>(rnd.size()), rnd.data());
    if (!ok) {
        CryptReleaseContext(hProv, 0);
        return false;
    }
    verifier = base64UrlEncode(rnd);

    HCRYPTHASH hHash = 0;
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return false;
    }
    CryptHashData(hHash, reinterpret_cast<const BYTE*>(verifier.data()), static_cast<DWORD>(verifier.size()), 0);
    DWORD hashLen = 32;
    std::vector<BYTE> digest(32);
    ok = CryptGetHashParam(hHash, HP_HASHVAL, digest.data(), &hashLen, 0);
    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    if (!ok) return false;

    challenge = base64UrlEncode(digest);
    return true;
}

std::string waitForOAuthCode(SOCKET listenSock) {
    SOCKET client = accept(listenSock, nullptr, nullptr);
    if (client == INVALID_SOCKET) return "";

    char buf[4096] = {0};
    int n = recv(client, buf, sizeof(buf) - 1, 0);
    std::string code;
    if (n > 0) {
        std::string req(buf, static_cast<size_t>(n));
        size_t pos = req.find("code=");
        if (pos != std::string::npos) {
            pos += 5;
            size_t end = req.find_first_of("& \r\n", pos);
            code = urlDecode(req.substr(pos, (end == std::string::npos) ? std::string::npos : end - pos));
        }
    }

    const char* html =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
        "<html><body style='font-family:sans-serif;text-align:center;margin-top:60px;'>"
        "<h2>&#10004; Google Drive Connected!</h2>"
        "<p>You can close this window and return to <b>snap.exe</b>.</p>"
        "</body></html>";
    send(client, html, static_cast<int>(std::strlen(html)), 0);
    closesocket(client);
    return code;
}

bool performOAuthLogin(AuthData& auth, const fs::path& authFile) {
    if (auth.clientId.empty()) {
        std::cout << "Enter Google OAuth Desktop Client ID: ";
        std::getline(std::cin, auth.clientId);
    }
    if (auth.clientSecret.empty()) {
        std::cout << "Enter Google OAuth Client Secret (press Enter if none): ";
        std::getline(std::cin, auth.clientSecret);
    }
    if (auth.clientId.empty()) {
        std::cerr << "Error: Client ID is required.\n";
        return false;
    }

    std::string verifier, challenge;
    if (!generatePkce(verifier, challenge)) {
        std::cerr << "Error: Failed to generate PKCE challenge.\n";
        return false;
    }

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "Error: WSAStartup failed.\n";
        return false;
    }

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        WSACleanup();
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // let OS pick free port
    if (bind(listenSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(listenSock, 1) == SOCKET_ERROR) {
        closesocket(listenSock);
        WSACleanup();
        return false;
    }

    int addrLen = sizeof(addr);
    getsockname(listenSock, reinterpret_cast<sockaddr*>(&addr), &addrLen);
    int port = ntohs(addr.sin_port);
    std::string redirectUri = "http://127.0.0.1:" + std::to_string(port);

    std::string authUrl =
        "https://accounts.google.com/o/oauth2/v2/auth"
        "?client_id=" + urlEncode(auth.clientId) +
        "&redirect_uri=" + urlEncode(redirectUri) +
        "&response_type=code"
        "&scope=" + urlEncode(DRIVE_SCOPE) +
        "&access_type=offline"
        "&prompt=consent"
        "&code_challenge=" + urlEncode(challenge) +
        "&code_challenge_method=S256";

    std::cout << "Opening browser for Google sign-in...\n";
    std::cout << "If the browser does not open, visit:\n" << authUrl << "\n\n";
    ShellExecuteW(nullptr, L"open", toWide(authUrl).c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    std::string code = waitForOAuthCode(listenSock);
    closesocket(listenSock);
    WSACleanup();

    if (code.empty()) {
        std::cerr << "Error: Did not receive authorization code.\n";
        return false;
    }

    std::string postBody =
        "code=" + urlEncode(code) +
        "&client_id=" + urlEncode(auth.clientId) +
        "&redirect_uri=" + urlEncode(redirectUri) +
        "&grant_type=authorization_code"
        "&code_verifier=" + urlEncode(verifier);
    if (!auth.clientSecret.empty()) {
        postBody += "&client_secret=" + urlEncode(auth.clientSecret);
    }

    HttpResponse resp = httpPost(
        L"oauth2.googleapis.com",
        L"/token",
        L"Content-Type: application/x-www-form-urlencoded\r\n",
        postBody.data(),
        static_cast<DWORD>(postBody.size())
    );

    if (!resp.networkOk || resp.statusCode != 200) {
        std::cerr << "Error exchanging token (HTTP " << resp.statusCode << "): " << resp.body << "\n";
        return false;
    }

    auth.accessToken = jsonExtractString(resp.body, "access_token");
    auth.refreshToken = jsonExtractString(resp.body, "refresh_token");
    long long expiresIn = jsonExtractInt(resp.body, "expires_in", 3600);
    auth.accessTokenExpiry = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(60LL, expiresIn - 60));

    if (auth.refreshToken.empty()) {
        std::cerr << "Error: No refresh_token returned by Google.\n";
        return false;
    }

    if (!saveAuthData(authFile, auth)) {
        std::cerr << "Error: Failed to save encrypted credentials.\n";
        return false;
    }

    std::cout << "Successfully connected to Google Drive!\n";
    return true;
}

static std::mutex g_authMtx;

bool ensureAccessToken(AuthData& auth, std::string& outToken) {
    std::lock_guard<std::mutex> lock(g_authMtx);
    auto now = std::chrono::steady_clock::now();
    if (!auth.accessToken.empty() && now < auth.accessTokenExpiry) {
        outToken = auth.accessToken;
        return true;
    }

    std::string postBody =
        "client_id=" + urlEncode(auth.clientId) +
        "&refresh_token=" + urlEncode(auth.refreshToken) +
        "&grant_type=refresh_token";
    if (!auth.clientSecret.empty()) {
        postBody += "&client_secret=" + urlEncode(auth.clientSecret);
    }

    HttpResponse resp = httpPost(
        L"oauth2.googleapis.com",
        L"/token",
        L"Content-Type: application/x-www-form-urlencoded\r\n",
        postBody.data(),
        static_cast<DWORD>(postBody.size())
    );

    if (!resp.networkOk || resp.statusCode != 200) {
        return false;
    }

    std::string newTok = jsonExtractString(resp.body, "access_token");
    if (newTok.empty()) return false;

    long long expiresIn = jsonExtractInt(resp.body, "expires_in", 3600);
    auth.accessToken = newTok;
    auth.accessTokenExpiry = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(60LL, expiresIn - 60));
    outToken = newTok;
    return true;
}

bool ensureDriveFolder(AuthData& auth, const fs::path& authFile, const std::string& folderName, std::string& outFolderId) {
    {
        std::lock_guard<std::mutex> lock(g_authMtx);
        if (!auth.driveFolderId.empty()) {
            outFolderId = auth.driveFolderId;
            return true;
        }
    }
    std::string token;
    if (!ensureAccessToken(auth, token)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_authMtx);
    if (!auth.driveFolderId.empty()) {
        outFolderId = auth.driveFolderId;
        return true;
    }

    std::string query = "name='" + folderName + "' and mimeType='application/vnd.google-apps.folder' and trashed=false";
    std::string searchPath = "/drive/v3/files?q=" + urlEncode(query) + "&fields=files(id,name)&pageSize=1";
    std::wstring authHeader = L"Authorization: Bearer " + toWide(token) + L"\r\n";

    HttpResponse searchResp = httpGet(L"www.googleapis.com", toWide(searchPath), authHeader);
    if (searchResp.networkOk && searchResp.statusCode == 200) {
        std::string existingId = jsonExtractString(searchResp.body, "id");
        if (!existingId.empty()) {
            auth.driveFolderId = existingId;
            saveAuthData(authFile, auth);
            outFolderId = existingId;
            return true;
        }
    }

    std::string createBody = "{\"name\":\"" + jsonEscape(folderName) +
                             "\",\"mimeType\":\"application/vnd.google-apps.folder\"}";
    std::wstring createHeaders =
        L"Authorization: Bearer " + toWide(token) + L"\r\n"
        L"Content-Type: application/json; charset=UTF-8\r\n";

    HttpResponse createResp = httpPost(
        L"www.googleapis.com",
        L"/drive/v3/files",
        createHeaders,
        createBody.data(),
        static_cast<DWORD>(createBody.size())
    );

    if (createResp.networkOk && (createResp.statusCode == 200 || createResp.statusCode == 201)) {
        std::string newId = jsonExtractString(createResp.body, "id");
        if (!newId.empty()) {
            auth.driveFolderId = newId;
            saveAuthData(authFile, auth);
            outFolderId = newId;
            return true;
        }
    }
    return false;
}

std::string extractDayName(const std::string& filename) {
    if (filename.size() >= 10 &&
        isdigit(static_cast<unsigned char>(filename[0])) &&
        filename[4] == '-' && filename[7] == '-') {
        return filename.substr(0, 10);
    }
    std::time_t now = std::time(nullptr);
    std::tm tmNow{};
    localtime_s(&tmNow, &now);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmNow);
    return std::string(buf);
}

bool ensureDaySubfolder(
    const std::string& parentId,
    const std::string& dayName,
    const std::string& token,
    std::string& outDayFolderId
) {
    if (parentId.empty() || dayName.empty()) return false;

    static std::mutex dayMtx;
    static std::string cachedParentId;
    static std::string cachedDayName;
    static std::string cachedDayId;

    std::lock_guard<std::mutex> lock(dayMtx);
    if (parentId == cachedParentId && dayName == cachedDayName && !cachedDayId.empty()) {
        outDayFolderId = cachedDayId;
        return true;
    }

    std::string query = "name='" + dayName + "' and '" + parentId +
                        "' in parents and mimeType='application/vnd.google-apps.folder' and trashed=false";
    std::string searchPath = "/drive/v3/files?q=" + urlEncode(query) + "&fields=files(id,name)&pageSize=1";
    std::wstring authHeader = L"Authorization: Bearer " + toWide(token) + L"\r\n";

    HttpResponse searchResp = httpGet(L"www.googleapis.com", toWide(searchPath), authHeader);
    if (searchResp.networkOk && searchResp.statusCode == 200) {
        std::string existingId = jsonExtractString(searchResp.body, "id");
        if (!existingId.empty()) {
            cachedParentId = parentId;
            cachedDayName = dayName;
            cachedDayId = existingId;
            outDayFolderId = existingId;
            return true;
        }
    }

    std::string createBody = "{\"name\":\"" + jsonEscape(dayName) +
                             "\",\"mimeType\":\"application/vnd.google-apps.folder\",\"parents\":[\"" +
                             jsonEscape(parentId) + "\"]}";
    std::wstring createHeaders =
        L"Authorization: Bearer " + toWide(token) + L"\r\n"
        L"Content-Type: application/json; charset=UTF-8\r\n";

    HttpResponse createResp = httpPost(
        L"www.googleapis.com",
        L"/drive/v3/files",
        createHeaders,
        createBody.data(),
        static_cast<DWORD>(createBody.size())
    );

    if (createResp.networkOk && (createResp.statusCode == 200 || createResp.statusCode == 201)) {
        std::string newId = jsonExtractString(createResp.body, "id");
        if (!newId.empty()) {
            cachedParentId = parentId;
            cachedDayName = dayName;
            cachedDayId = newId;
            outDayFolderId = newId;
            return true;
        }
    }
    return false;
}

bool uploadBytesToDrive(
    AuthData& auth,
    const fs::path& authFile,
    const std::string& folderName,
    const std::string& filename,
    const std::string& mimeType,
    const std::vector<BYTE>& imageBytes
) {
    std::string token;
    if (!ensureAccessToken(auth, token)) {
        return false;
    }
    std::string folderId;
    ensureDriveFolder(auth, authFile, folderName, folderId);

    std::string targetFolderId = folderId;
    if (!folderId.empty()) {
        std::string dayFolderId;
        if (ensureDaySubfolder(folderId, extractDayName(filename), token, dayFolderId)) {
            targetFolderId = dayFolderId;
        }
    }

    std::string metadata = "{\"name\":\"" + jsonEscape(filename) + "\"";
    if (!targetFolderId.empty()) {
        metadata += ",\"parents\":[\"" + jsonEscape(targetFolderId) + "\"]";
    }
    metadata += "}";

    std::string boundary = "snap_drive_boundary_9f8e7d6c";
    std::string part1 =
        "--" + boundary + "\r\n"
        "Content-Type: application/json; charset=UTF-8\r\n\r\n" +
        metadata + "\r\n"
        "--" + boundary + "\r\n"
        "Content-Type: " + mimeType + "\r\n\r\n";
    std::string part3 = "\r\n--" + boundary + "--\r\n";

    std::vector<BYTE> body;
    body.reserve(part1.size() + imageBytes.size() + part3.size());
    body.insert(body.end(), part1.begin(), part1.end());
    body.insert(body.end(), imageBytes.begin(), imageBytes.end());
    body.insert(body.end(), part3.begin(), part3.end());

    std::wstring headers =
        L"Authorization: Bearer " + toWide(token) + L"\r\n"
        L"Content-Type: multipart/related; boundary=" + toWide(boundary) + L"\r\n";

    HttpResponse resp = httpPost(
        L"www.googleapis.com",
        L"/upload/drive/v3/files?uploadType=multipart",
        headers,
        body.data(),
        static_cast<DWORD>(body.size())
    );

    if (resp.statusCode == 401) {
        {
            std::lock_guard<std::mutex> lock(g_authMtx);
            auth.accessToken.clear();
        }
        if (ensureAccessToken(auth, token)) {
            headers =
                L"Authorization: Bearer " + toWide(token) + L"\r\n"
                L"Content-Type: multipart/related; boundary=" + toWide(boundary) + L"\r\n";
            resp = httpPost(
                L"www.googleapis.com",
                L"/upload/drive/v3/files?uploadType=multipart",
                headers,
                body.data(),
                static_cast<DWORD>(body.size())
            );
        }
    }

    return resp.networkOk && (resp.statusCode == 200 || resp.statusCode == 201);
}

// MARK: - Google Drive Full Cleanup (Keep Only Images Inside App Folder)

struct DriveFileInfo {
    std::string id;
    std::string name;
    std::string mimeType;
    std::vector<std::string> parents;
};

static void jsonSkipWs(const std::string& s, size_t& pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n')) {
        ++pos;
    }
}

static bool jsonParseStringAt(const std::string& s, size_t& pos, std::string& out) {
    jsonSkipWs(s, pos);
    if (pos >= s.size() || s[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < s.size()) {
        char c = s[pos++];
        if (c == '\\' && pos < s.size()) {
            char esc = s[pos++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u':
                    if (pos + 4 <= s.size()) pos += 4;
                    break;
                default: out.push_back(esc); break;
            }
        } else if (c == '"') {
            return true;
        } else {
            out.push_back(c);
        }
    }
    return false;
}

static void jsonSkipValue(const std::string& s, size_t& pos) {
    jsonSkipWs(s, pos);
    if (pos >= s.size()) return;
    char c = s[pos];
    if (c == '"') {
        std::string dummy;
        jsonParseStringAt(s, pos, dummy);
    } else if (c == '{') {
        ++pos;
        jsonSkipWs(s, pos);
        if (pos < s.size() && s[pos] == '}') { ++pos; return; }
        while (pos < s.size()) {
            std::string k;
            if (!jsonParseStringAt(s, pos, k)) break;
            jsonSkipWs(s, pos);
            if (pos < s.size() && s[pos] == ':') ++pos;
            jsonSkipValue(s, pos);
            jsonSkipWs(s, pos);
            if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
            if (pos < s.size() && s[pos] == '}') { ++pos; break; }
            break;
        }
    } else if (c == '[') {
        ++pos;
        jsonSkipWs(s, pos);
        if (pos < s.size() && s[pos] == ']') { ++pos; return; }
        while (pos < s.size()) {
            jsonSkipValue(s, pos);
            jsonSkipWs(s, pos);
            if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
            if (pos < s.size() && s[pos] == ']') { ++pos; break; }
            break;
        }
    } else {
        while (pos < s.size() && s[pos] != ',' && s[pos] != '}' && s[pos] != ']' &&
               s[pos] != ' ' && s[pos] != '\t' && s[pos] != '\r' && s[pos] != '\n') {
            ++pos;
        }
    }
}

static bool parseDriveFilesListResponse(
    const std::string& json,
    std::vector<DriveFileInfo>& outFiles,
    std::string& outNextPageToken
) {
    outNextPageToken.clear();
    size_t pos = 0;
    jsonSkipWs(json, pos);
    if (pos >= json.size() || json[pos] != '{') return false;
    ++pos;

    while (pos < json.size()) {
        jsonSkipWs(json, pos);
        if (pos < json.size() && json[pos] == '}') { ++pos; return true; }
        std::string key;
        if (!jsonParseStringAt(json, pos, key)) return false;
        jsonSkipWs(json, pos);
        if (pos >= json.size() || json[pos] != ':') return false;
        ++pos;
        jsonSkipWs(json, pos);

        if (key == "nextPageToken") {
            jsonParseStringAt(json, pos, outNextPageToken);
        } else if (key == "files") {
            if (pos < json.size() && json[pos] == '[') {
                ++pos;
                jsonSkipWs(json, pos);
                while (pos < json.size() && json[pos] != ']') {
                    jsonSkipWs(json, pos);
                    if (pos < json.size() && json[pos] == '{') {
                        ++pos;
                        DriveFileInfo info;
                        while (pos < json.size()) {
                            jsonSkipWs(json, pos);
                            if (pos < json.size() && json[pos] == '}') { ++pos; break; }
                            std::string fkey;
                            if (!jsonParseStringAt(json, pos, fkey)) break;
                            jsonSkipWs(json, pos);
                            if (pos < json.size() && json[pos] == ':') ++pos;
                            jsonSkipWs(json, pos);
                            if (fkey == "id") {
                                jsonParseStringAt(json, pos, info.id);
                            } else if (fkey == "name") {
                                jsonParseStringAt(json, pos, info.name);
                            } else if (fkey == "mimeType") {
                                jsonParseStringAt(json, pos, info.mimeType);
                            } else if (fkey == "parents" && pos < json.size() && json[pos] == '[') {
                                ++pos;
                                jsonSkipWs(json, pos);
                                while (pos < json.size() && json[pos] != ']') {
                                    std::string pid;
                                    if (jsonParseStringAt(json, pos, pid)) {
                                        info.parents.push_back(std::move(pid));
                                    } else {
                                        jsonSkipValue(json, pos);
                                    }
                                    jsonSkipWs(json, pos);
                                    if (pos < json.size() && json[pos] == ',') ++pos;
                                    jsonSkipWs(json, pos);
                                }
                                if (pos < json.size() && json[pos] == ']') ++pos;
                            } else {
                                jsonSkipValue(json, pos);
                            }
                            jsonSkipWs(json, pos);
                            if (pos < json.size() && json[pos] == ',') ++pos;
                        }
                        if (!info.id.empty()) {
                            outFiles.push_back(std::move(info));
                        }
                    } else {
                        jsonSkipValue(json, pos);
                    }
                    jsonSkipWs(json, pos);
                    if (pos < json.size() && json[pos] == ',') ++pos;
                    jsonSkipWs(json, pos);
                }
                if (pos < json.size() && json[pos] == ']') ++pos;
            } else {
                jsonSkipValue(json, pos);
            }
        } else {
            jsonSkipValue(json, pos);
        }

        jsonSkipWs(json, pos);
        if (pos < json.size() && json[pos] == ',') ++pos;
    }
    return true;
}

static bool verifyFolderExistsAndActive(const std::string& folderId, const std::string& token) {
    if (folderId.empty()) return false;
    std::string path = "/drive/v3/files/" + urlEncode(folderId) + "?fields=id,trashed";
    std::wstring authHeader = L"Authorization: Bearer " + toWide(token) + L"\r\n";
    HttpResponse resp = httpGet(L"www.googleapis.com", toWide(path), authHeader);
    if (!resp.networkOk || resp.statusCode != 200) return false;
    if (resp.body.find("\"trashed\": true") != std::string::npos ||
        resp.body.find("\"trashed\":true") != std::string::npos) {
        return false;
    }
    return !jsonExtractString(resp.body, "id").empty();
}

bool cleanGoogleDrive(
    AuthData& auth,
    const fs::path& authFile,
    const std::string& folderName,
    bool assumeYes
) {
    std::string token;
    if (!ensureAccessToken(auth, token)) {
        std::cerr << "Error: Could not obtain Google Drive access token.\n";
        return false;
    }

    // Verify saved driveFolderId actually exists and is not trashed
    if (!auth.driveFolderId.empty() && !verifyFolderExistsAndActive(auth.driveFolderId, token)) {
        std::lock_guard<std::mutex> lock(g_authMtx);
        auth.driveFolderId.clear();
    }

    std::string appFolderId;
    if (!ensureDriveFolder(auth, authFile, folderName, appFolderId) || appFolderId.empty()) {
        std::cerr << "Error: Could not locate or create the app's Google Drive folder ('"
                  << folderName << "'). Aborting cleanup to protect your files.\n";
        return false;
    }

    std::cout << "Scanning Google Drive...\n";
    std::vector<DriveFileInfo> allFiles;
    std::string pageToken;
    const std::string query = "'me' in owners and trashed=false";

    while (true) {
        std::string path = "/drive/v3/files?q=" + urlEncode(query) +
                           "&fields=nextPageToken,files(id,name,mimeType,parents)&pageSize=1000";
        if (!pageToken.empty()) {
            path += "&pageToken=" + urlEncode(pageToken);
        }
        std::wstring authHeader = L"Authorization: Bearer " + toWide(token) + L"\r\n";
        HttpResponse resp = httpGet(L"www.googleapis.com", toWide(path), authHeader);

        if (resp.statusCode == 401) {
            {
                std::lock_guard<std::mutex> lock(g_authMtx);
                auth.accessToken.clear();
            }
            if (!ensureAccessToken(auth, token)) {
                std::cerr << "Error: Failed to refresh access token.\n";
                return false;
            }
            authHeader = L"Authorization: Bearer " + toWide(token) + L"\r\n";
            resp = httpGet(L"www.googleapis.com", toWide(path), authHeader);
        }

        if (!resp.networkOk || resp.statusCode != 200) {
            if (resp.statusCode == 403) {
                std::cerr << "Error (HTTP 403): Insufficient Google Drive permissions.\n"
                          << "Please re-authenticate with full Drive access:\n"
                          << "  snap.exe --logout\n"
                          << "  snap.exe --login\n";
            } else {
                std::cerr << "Error listing Google Drive files (HTTP " << resp.statusCode << "): "
                          << resp.body << "\n";
            }
            return false;
        }

        std::string nextTok;
        if (!parseDriveFilesListResponse(resp.body, allFiles, nextTok)) {
            std::cerr << "Error: Failed to parse Google Drive file list.\n";
            return false;
        }
        if (nextTok.empty()) break;
        pageToken = nextTok;
    }

    std::unordered_map<std::string, const DriveFileInfo*> fileMap;
    fileMap.reserve(allFiles.size());
    for (const auto& f : allFiles) {
        fileMap[f.id] = &f;
    }

    // Protect appFolderId and any of its ancestor folders
    std::unordered_set<std::string> protectedAncestors;
    protectedAncestors.insert(appFolderId);
    {
        std::vector<std::string> stack{appFolderId};
        while (!stack.empty()) {
            std::string cur = stack.back();
            stack.pop_back();
            auto it = fileMap.find(cur);
            if (it == fileMap.end()) continue;
            for (const auto& p : it->second->parents) {
                if (protectedAncestors.insert(p).second) {
                    stack.push_back(p);
                }
            }
        }
    }

    auto isInsideAppFolder = [&](const std::string& id) -> bool {
        std::unordered_set<std::string> visited;
        std::vector<std::string> stack;
        auto it0 = fileMap.find(id);
        if (it0 == fileMap.end()) return false;
        for (const auto& p : it0->second->parents) stack.push_back(p);
        while (!stack.empty()) {
            std::string cur = stack.back();
            stack.pop_back();
            if (cur == appFolderId) return true;
            if (!visited.insert(cur).second) continue;
            auto it = fileMap.find(cur);
            if (it != fileMap.end()) {
                for (const auto& p : it->second->parents) stack.push_back(p);
            }
        }
        return false;
    };

    std::unordered_set<std::string> toDeleteIds;
    size_t keptImages = 0;
    size_t keptSubfolders = 0;

    for (const auto& f : allFiles) {
        if (protectedAncestors.count(f.id)) {
            continue;
        }
        if (isInsideAppFolder(f.id)) {
            if (f.mimeType.rfind("image/", 0) == 0) {
                ++keptImages;
                continue;
            }
            if (f.mimeType == "application/vnd.google-apps.folder") {
                ++keptSubfolders;
                continue;
            }
            // Non-image file inside the app folder -> delete
            toDeleteIds.insert(f.id);
        } else {
            toDeleteIds.insert(f.id);
        }
    }

    // Only issue DELETE for top-level targets whose parents are not all already in toDeleteIds
    std::vector<const DriveFileInfo*> deleteTargets;
    for (const auto& f : allFiles) {
        if (!toDeleteIds.count(f.id)) continue;
        bool coveredByDeletedParent = false;
        if (!f.parents.empty()) {
            coveredByDeletedParent = true;
            for (const auto& p : f.parents) {
                if (!toDeleteIds.count(p)) {
                    coveredByDeletedParent = false;
                    break;
                }
            }
        }
        if (!coveredByDeletedParent) {
            deleteTargets.push_back(&f);
        }
    }

    std::cout << "\n=== Google Drive Cleanup Summary ===\n";
    std::cout << "  Protected folder : " << folderName << " (ID: " << appFolderId << ")\n";
    std::cout << "  Images kept      : " << keptImages << " (in " << keptSubfolders << " subfolder(s))\n";
    std::cout << "  Items to delete  : " << toDeleteIds.size()
              << " (" << deleteTargets.size() << " top-level file(s)/folder(s))\n";
    std::cout << "  Trash            : Will be permanently emptied\n\n";

    if (!assumeYes) {
        std::cout << "WARNING: This will PERMANENTLY delete everything in Google Drive\n"
                  << "outside '" << folderName << "' (and empty Trash). This cannot be undone!\n"
                  << "Type YES to confirm: ";
        std::string confirm;
        std::getline(std::cin, confirm);
        if (confirm != "YES" && confirm != "yes") {
            std::cout << "Cleanup cancelled.\n";
            return false;
        }
    }

    size_t deletedCount = 0;
    size_t failedCount = 0;
    bool scopeError = false;

    for (size_t i = 0; i < deleteTargets.size(); ++i) {
        const DriveFileInfo* item = deleteTargets[i];
        std::string delPath = "/drive/v3/files/" + urlEncode(item->id);
        std::wstring authHeader = L"Authorization: Bearer " + toWide(token) + L"\r\n";
        HttpResponse resp = httpDelete(L"www.googleapis.com", toWide(delPath), authHeader);

        if (resp.statusCode == 401) {
            {
                std::lock_guard<std::mutex> lock(g_authMtx);
                auth.accessToken.clear();
            }
            if (ensureAccessToken(auth, token)) {
                authHeader = L"Authorization: Bearer " + toWide(token) + L"\r\n";
                resp = httpDelete(L"www.googleapis.com", toWide(delPath), authHeader);
            }
        }

        if (resp.networkOk && (resp.statusCode == 200 || resp.statusCode == 204 || resp.statusCode == 404)) {
            ++deletedCount;
            std::cout << "[" << (i + 1) << "/" << deleteTargets.size() << "] Deleted: " << item->name << "\n";
        } else {
            ++failedCount;
            if (resp.statusCode == 403) {
                scopeError = true;
            }
            std::cerr << "[" << (i + 1) << "/" << deleteTargets.size() << "] Failed to delete '"
                      << item->name << "' (HTTP " << resp.statusCode << ")\n";
        }
    }

    // Empty Google Drive Trash permanently
    std::cout << "Emptying Google Drive Trash...\n";
    std::wstring trashAuthHeader = L"Authorization: Bearer " + toWide(token) + L"\r\n";
    HttpResponse trashResp = httpDelete(L"www.googleapis.com", L"/drive/v3/files/trash", trashAuthHeader);
    if (trashResp.statusCode == 401) {
        {
            std::lock_guard<std::mutex> lock(g_authMtx);
            auth.accessToken.clear();
        }
        if (ensureAccessToken(auth, token)) {
            trashAuthHeader = L"Authorization: Bearer " + toWide(token) + L"\r\n";
            trashResp = httpDelete(L"www.googleapis.com", L"/drive/v3/files/trash", trashAuthHeader);
        }
    }

    bool trashOk = trashResp.networkOk && (trashResp.statusCode == 200 || trashResp.statusCode == 204);
    if (trashOk) {
        std::cout << "Trash emptied permanently.\n";
    } else {
        if (trashResp.statusCode == 403) scopeError = true;
        std::cerr << "Warning: Could not empty Trash (HTTP " << trashResp.statusCode << ").\n";
    }

    if (scopeError) {
        std::cerr << "\nNote: Some operations returned HTTP 403 (insufficient permissions).\n"
                  << "Run `snap.exe --logout` then `snap.exe --login` to grant full Drive access.\n";
    }

    std::cout << "\nCleanup complete. Deleted: " << deletedCount
              << ", Failed: " << failedCount
              << ", Kept images: " << keptImages << ".\n";
    return (failedCount == 0 && trashOk);
}

// MARK: - Windows Startup Registry Helpers

bool installStartupEntry(const Options& opts, const fs::path& exePath, const fs::path& resolvedOutDir) {
    HKEY hKey = nullptr;
    LONG res = RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_SET_VALUE, &hKey);
    if (res != ERROR_SUCCESS) {
        std::cerr << "Error: Failed to open Windows Startup registry key (code " << res << ").\n";
        return false;
    }

    std::wostringstream cmd;
    cmd << L"\"" << exePath.wstring() << L"\""
        << L" -i " << opts.intervalSeconds
        << L" --diff " << opts.diffThresholdPercent
        << L" -f " << toWide(opts.format)
        << L" -q " << opts.jpegQuality;
    if (opts.localOnly) {
        cmd << L" --local -o \"" << resolvedOutDir.wstring() << L"\"";
    }
    if (!opts.driveFolderId.empty()) {
        cmd << L" --folder \"" << toWide(opts.driveFolderId) << L"\"";
    }
    if (opts.displayNumber >= 1) {
        cmd << L" -d " << opts.displayNumber;
    }
    if (opts.background) {
        cmd << L" --background";
    }

    std::wstring cmdStr = cmd.str();
    DWORD byteSize = static_cast<DWORD>((cmdStr.size() + 1) * sizeof(wchar_t));
    res = RegSetValueExW(hKey, REG_VALUE_NAME, 0, REG_SZ,
                         reinterpret_cast<const BYTE*>(cmdStr.c_str()), byteSize);
    RegCloseKey(hKey);

    if (res != ERROR_SUCCESS) {
        std::cerr << "Error: Failed to write Windows Startup registry value (code " << res << ").\n";
        return false;
    }

    std::wcout << L"Startup enabled: " << cmdStr << L"\n";
    return true;
}

bool removeStartupEntry() {
    HKEY hKey = nullptr;
    LONG res = RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_SET_VALUE, &hKey);
    if (res != ERROR_SUCCESS) return false;
    res = RegDeleteValueW(hKey, REG_VALUE_NAME);
    RegCloseKey(hKey);
    if (res == ERROR_FILE_NOT_FOUND) {
        std::cout << "Startup was already disabled.\n";
        return true;
    }
    if (res != ERROR_SUCCESS) return false;
    std::cout << "Startup disabled.\n";
    return true;
}

void printStartupStatus() {
    HKEY hKey = nullptr;
    LONG res = RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_QUERY_VALUE, &hKey);
    if (res != ERROR_SUCCESS) return;

    wchar_t buf[2048] = {0};
    DWORD bufSize = sizeof(buf);
    DWORD type = 0;
    res = RegQueryValueExW(hKey, REG_VALUE_NAME, nullptr, &type, reinterpret_cast<BYTE*>(buf), &bufSize);
    RegCloseKey(hKey);

    if (res == ERROR_SUCCESS && type == REG_SZ) {
        std::wcout << L"Startup: ENABLED\nCommand: " << buf << L"\n";
    } else {
        std::cout << "Startup: DISABLED\n";
    }
}

// MARK: - Screen Capture & Diff

static const int DIFF_W = 640;
static const int DIFF_H = 360;

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;

    std::vector<BYTE> buffer(size);
    auto* pImageCodecInfo = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);

    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            return static_cast<int>(j);
        }
    }
    return -1;
}

BOOL CALLBACK MonitorEnumProc(HMONITOR, HDC, LPRECT lprcMonitor, LPARAM dwData) {
    auto* monitors = reinterpret_cast<std::vector<RECT>*>(dwData);
    monitors->push_back(*lprcMonitor);
    return TRUE;
}

RECT getCaptureRect(int displayNumber, bool& ok) {
    ok = true;
    if (displayNumber == 0) {
        return RECT{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    }
    std::vector<RECT> monitors;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&monitors));
    if (displayNumber < 1 || static_cast<size_t>(displayNumber) > monitors.size()) {
        std::cerr << "Error: Display " << displayNumber << " not found.\n";
        ok = false;
        return RECT{0, 0, 0, 0};
    }
    return monitors[static_cast<size_t>(displayNumber - 1)];
}

std::string makeTimestampFilename(const fs::path& checkDir, const std::string& format) {
    static std::mutex nameMtx;
    static std::string lastTs;
    static int sameSecSeq = 0;
    std::lock_guard<std::mutex> lock(nameMtx);

    std::time_t now = std::time(nullptr);
    std::tm tmNow{};
    localtime_s(&tmNow, &now);
    char buf[40];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %I.%M.%S %p", &tmNow);

    std::string ts(buf);
    if (ts == lastTs) {
        ++sameSecSeq;
    } else {
        lastTs = ts;
        sameSecSeq = 0;
    }

    std::string base = (sameSecSeq == 0)
        ? ts
        : (ts + " (" + std::to_string(sameSecSeq + 1) + ")");
    std::string name = base + "." + format;
    int counter = 2;
    std::error_code ec;
    while (!checkDir.empty() && fs::exists(checkDir / name, ec)) {
        name = base + " (" + std::to_string(counter) + ")." + format;
        ++counter;
    }
    return name;
}

bool captureThumbOnly(const RECT& rect, std::vector<BYTE>& outThumbBGRA) {
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return false;

    HDC hScreenDC = GetDC(nullptr);
    HDC hThumbDC = CreateCompatibleDC(hScreenDC);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = DIFF_W;
    bmi.bmiHeader.biHeight = -DIFF_H;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* thumbBits = nullptr;
    HBITMAP hThumbBmp = CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, &thumbBits, nullptr, 0);
    bool ok = false;
    if (hThumbBmp && thumbBits) {
        HGDIOBJ oldThumb = SelectObject(hThumbDC, hThumbBmp);
        SetStretchBltMode(hThumbDC, COLORONCOLOR);
        if (StretchBlt(hThumbDC, 0, 0, DIFF_W, DIFF_H, hScreenDC, rect.left, rect.top, width, height, SRCCOPY | CAPTUREBLT)) {
            outThumbBGRA.assign(
                static_cast<BYTE*>(thumbBits),
                static_cast<BYTE*>(thumbBits) + (DIFF_W * DIFF_H * 4)
            );
            ok = true;
        }
        SelectObject(hThumbDC, oldThumb);
        DeleteObject(hThumbBmp);
    }

    DeleteDC(hThumbDC);
    ReleaseDC(nullptr, hScreenDC);
    return ok;
}

bool captureToMemoryAndThumb(
    const RECT& rect,
    const CLSID& encoderClsid,
    int jpegQuality,
    bool isJpeg,
    std::vector<BYTE>& outImageBytes,
    std::vector<BYTE>& outThumbBGRA
) {
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return false;

    HDC hScreenDC = GetDC(nullptr);
    HDC hMemoryDC = CreateCompatibleDC(hScreenDC);
    HBITMAP hBitmap = CreateCompatibleBitmap(hScreenDC, width, height);
    HGDIOBJ oldObj = SelectObject(hMemoryDC, hBitmap);

    BOOL bltOk = BitBlt(hMemoryDC, 0, 0, width, height, hScreenDC, rect.left, rect.top, SRCCOPY | CAPTUREBLT);

    // Create 160x90 thumbnail for fast frame-diffing
    if (bltOk) {
        HDC hThumbDC = CreateCompatibleDC(hScreenDC);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = DIFF_W;
        bmi.bmiHeader.biHeight = -DIFF_H;  // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* thumbBits = nullptr;
        HBITMAP hThumbBmp = CreateDIBSection(hScreenDC, &bmi, DIB_RGB_COLORS, &thumbBits, nullptr, 0);
        if (hThumbBmp && thumbBits) {
            HGDIOBJ oldThumb = SelectObject(hThumbDC, hThumbBmp);
            SetStretchBltMode(hThumbDC, COLORONCOLOR);
            StretchBlt(hThumbDC, 0, 0, DIFF_W, DIFF_H, hMemoryDC, 0, 0, width, height, SRCCOPY);
            outThumbBGRA.assign(
                static_cast<BYTE*>(thumbBits),
                static_cast<BYTE*>(thumbBits) + (DIFF_W * DIFF_H * 4)
            );
            SelectObject(hThumbDC, oldThumb);
            DeleteObject(hThumbBmp);
        }
        DeleteDC(hThumbDC);
    }

    SelectObject(hMemoryDC, oldObj);
    DeleteDC(hMemoryDC);
    ReleaseDC(nullptr, hScreenDC);

    if (!bltOk) {
        DeleteObject(hBitmap);
        return false;
    }

    IStream* memStream = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &memStream) != S_OK || !memStream) {
        DeleteObject(hBitmap);
        return false;
    }

    Gdiplus::Bitmap bitmap(hBitmap, nullptr);
    Gdiplus::EncoderParameters encoderParams{};
    ULONG qualityVal = static_cast<ULONG>(jpegQuality);
    Gdiplus::Status status;
    if (isJpeg) {
        encoderParams.Count = 1;
        encoderParams.Parameter[0].Guid = Gdiplus::EncoderQuality;
        encoderParams.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        encoderParams.Parameter[0].NumberOfValues = 1;
        encoderParams.Parameter[0].Value = &qualityVal;
        status = bitmap.Save(memStream, &encoderClsid, &encoderParams);
    } else {
        status = bitmap.Save(memStream, &encoderClsid, nullptr);
    }

    DeleteObject(hBitmap);

    if (status == Gdiplus::Ok) {
        HGLOBAL hMem = nullptr;
        if (GetHGlobalFromStream(memStream, &hMem) == S_OK && hMem) {
            SIZE_T sz = GlobalSize(hMem);
            void* ptr = GlobalLock(hMem);
            if (ptr && sz > 0) {
                outImageBytes.assign(static_cast<BYTE*>(ptr), static_cast<BYTE*>(ptr) + sz);
            }
            GlobalUnlock(hMem);
        }
    }

    memStream->Release();
    return !outImageBytes.empty();
}

double computeDiffPercent(const std::vector<BYTE>& prevThumb, const std::vector<BYTE>& currThumb) {
    if (prevThumb.empty() || prevThumb.size() != currThumb.size()) {
        return 100.0;
    }
    // Exclude bottom 6% (Windows taskbar, system tray, clock, and toast area)
    const int activeRows = (DIFF_H * 94) / 100;
    const size_t activePixels = static_cast<size_t>(DIFF_W) * static_cast<size_t>(activeRows);
    const size_t activeBytes = activePixels * 4;
    if (activeBytes == 0 || activeBytes > prevThumb.size()) {
        return 100.0;
    }

    size_t changedPixels = 0;
    const int channelThreshold = 10;

    for (size_t i = 0; i < activeBytes; i += 4) {
        int db = std::abs(static_cast<int>(prevThumb[i]) - static_cast<int>(currThumb[i]));
        int dg = std::abs(static_cast<int>(prevThumb[i + 1]) - static_cast<int>(currThumb[i + 1]));
        int dr = std::abs(static_cast<int>(prevThumb[i + 2]) - static_cast<int>(currThumb[i + 2]));
        if (db > channelThreshold || dg > channelThreshold || dr > channelThreshold) {
            ++changedPixels;
        }
    }
    return (static_cast<double>(changedPixels) * 100.0) / static_cast<double>(activePixels);
}

// MARK: - DXGI Desktop Duplication (OS Event-Driven Screen Capture)

class DxgiDuplicator {
public:
    DxgiDuplicator(int displayNumber) : displayIndex_(displayNumber >= 1 ? displayNumber - 1 : 0) {}

    ~DxgiDuplicator() {
        cleanup();
    }

    bool init() {
        cleanup();

        D3D_FEATURE_LEVEL fl{};
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION,
            &device_, &fl, &context_
        );
        if (FAILED(hr) || !device_ || !context_) {
            return false;
        }

        IDXGIDevice* dxgiDevice = nullptr;
        hr = device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
        if (FAILED(hr) || !dxgiDevice) return false;

        IDXGIAdapter* adapter = nullptr;
        hr = dxgiDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&adapter));
        dxgiDevice->Release();
        if (FAILED(hr) || !adapter) return false;

        IDXGIOutput* output = nullptr;
        hr = adapter->EnumOutputs(static_cast<UINT>(displayIndex_), &output);
        adapter->Release();
        if (FAILED(hr) || !output) return false;

        IDXGIOutput1* output1 = nullptr;
        hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
        output->Release();
        if (FAILED(hr) || !output1) return false;

        hr = output1->DuplicateOutput(device_, &dupl_);
        output1->Release();
        if (FAILED(hr) || !dupl_) return false;

        DXGI_OUTDUPL_DESC desc{};
        dupl_->GetDesc(&desc);
        width_ = static_cast<int>(desc.ModeDesc.Width);
        height_ = static_cast<int>(desc.ModeDesc.Height);

        D3D11_TEXTURE2D_DESC texDesc{};
        texDesc.Width = desc.ModeDesc.Width;
        texDesc.Height = desc.ModeDesc.Height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_STAGING;
        texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        hr = device_->CreateTexture2D(&texDesc, nullptr, &staging_);
        if (FAILED(hr) || !staging_) return false;

        initialized_ = true;
        return true;
    }

    // Returns: 1 = captured new image, 0 = no event/no change, -1 = access lost/error
    int waitForEventAndCapture(
        UINT timeoutMs,
        bool forceCapture,
        double diffThresholdPercent,
        const CLSID& encoderClsid,
        int jpegQuality,
        bool isJpeg,
        std::vector<BYTE>& prevSample,
        std::vector<BYTE>& outImageBytes
    ) {
        if (!initialized_ && !init()) {
            return -1;
        }

        IDXGIResource* desktopRes = nullptr;
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        HRESULT hr = dupl_->AcquireNextFrame(timeoutMs, &frameInfo, &desktopRes);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            return 0;  // No OS screen-change event
        }
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
            if (desktopRes) desktopRes->Release();
            initialized_ = false;
            return -1;
        }
        if (FAILED(hr) || !desktopRes) {
            if (desktopRes) desktopRes->Release();
            return -1;
        }

        if (!forceCapture && frameInfo.LastPresentTime.QuadPart == 0 && frameInfo.AccumulatedFrames == 0) {
            desktopRes->Release();
            dupl_->ReleaseFrame();
            return 0;  // Only pointer/metadata update, no desktop pixel change
        }

        ID3D11Texture2D* tex = nullptr;
        hr = desktopRes->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
        desktopRes->Release();
        if (FAILED(hr) || !tex) {
            dupl_->ReleaseFrame();
            return -1;
        }

        context_->CopyResource(staging_, tex);
        tex->Release();

        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = context_->Map(staging_, 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) {
            dupl_->ReleaseFrame();
            return -1;
        }

        const BYTE* basePtr = static_cast<const BYTE*>(mapped.pData);
        std::vector<BYTE> currSample(DIFF_W * DIFF_H * 4);
        size_t sIdx = 0;
        for (int gy = 0; gy < DIFF_H; ++gy) {
            int sy = (gy * height_) / DIFF_H;
            const BYTE* row = basePtr + (static_cast<size_t>(sy) * mapped.RowPitch);
            for (int gx = 0; gx < DIFF_W; ++gx) {
                int sx = (gx * width_) / DIFF_W;
                const BYTE* px = row + (static_cast<size_t>(sx) * 4);
                currSample[sIdx++] = px[0];
                currSample[sIdx++] = px[1];
                currSample[sIdx++] = px[2];
                currSample[sIdx++] = 255;
            }
        }

        bool shouldEncode = forceCapture || prevSample.empty() || (diffThresholdPercent <= 0.0);
        if (!shouldEncode) {
            double diffPct = computeDiffPercent(prevSample, currSample);
            if (diffPct >= diffThresholdPercent) {
                shouldEncode = true;
            }
        }

        int result = 0;
        if (shouldEncode) {
            prevSample = std::move(currSample);
            if (encodeMappedBGRA(basePtr, mapped.RowPitch, encoderClsid, jpegQuality, isJpeg, outImageBytes)) {
                result = 1;
            }
        }

        context_->Unmap(staging_, 0);
        dupl_->ReleaseFrame();
        return result;
    }

private:
    bool encodeMappedBGRA(
        const BYTE* data,
        UINT rowPitch,
        const CLSID& encoderClsid,
        int jpegQuality,
        bool isJpeg,
        std::vector<BYTE>& outBytes
    ) {
        IStream* memStream = nullptr;
        if (CreateStreamOnHGlobal(nullptr, TRUE, &memStream) != S_OK || !memStream) {
            return false;
        }

        Gdiplus::Bitmap bitmap(
            width_, height_, static_cast<INT>(rowPitch),
            PixelFormat32bppARGB, const_cast<BYTE*>(data)
        );
        Gdiplus::Status status;
        if (isJpeg) {
            Gdiplus::EncoderParameters params{};
            ULONG q = static_cast<ULONG>(jpegQuality);
            params.Count = 1;
            params.Parameter[0].Guid = Gdiplus::EncoderQuality;
            params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
            params.Parameter[0].NumberOfValues = 1;
            params.Parameter[0].Value = &q;
            status = bitmap.Save(memStream, &encoderClsid, &params);
        } else {
            status = bitmap.Save(memStream, &encoderClsid, nullptr);
        }

        if (status == Gdiplus::Ok) {
            HGLOBAL hMem = nullptr;
            if (GetHGlobalFromStream(memStream, &hMem) == S_OK && hMem) {
                SIZE_T sz = GlobalSize(hMem);
                void* ptr = GlobalLock(hMem);
                if (ptr && sz > 0) {
                    outBytes.assign(static_cast<BYTE*>(ptr), static_cast<BYTE*>(ptr) + sz);
                }
                GlobalUnlock(hMem);
            }
        }
        memStream->Release();
        return !outBytes.empty();
    }

    void cleanup() {
        if (staging_) { staging_->Release(); staging_ = nullptr; }
        if (dupl_) { dupl_->Release(); dupl_ = nullptr; }
        if (context_) { context_->Release(); context_ = nullptr; }
        if (device_) { device_->Release(); device_ = nullptr; }
        initialized_ = false;
    }

    int displayIndex_;
    int width_ = 0;
    int height_ = 0;
    bool initialized_ = false;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGIOutputDuplication* dupl_ = nullptr;
    ID3D11Texture2D* staging_ = nullptr;
};

bool writeBytesToFile(const fs::path& path, const std::vector<BYTE>& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return f.good();
}

bool readBytesFromFile(const fs::path& path, std::vector<BYTE>& bytes) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    bytes.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return !bytes.empty();
}

void flushPendingQueue(
    const fs::path& queueDir,
    AuthData& auth,
    const fs::path& authFile,
    const std::string& folderName,
    bool quiet
) {
    std::error_code ec;
    if (!fs::exists(queueDir, ec)) return;
    std::vector<fs::path> pending;
    for (const auto& entry : fs::directory_iterator(queueDir, ec)) {
        if (ec) break;
        if (entry.is_regular_file(ec)) {
            pending.push_back(entry.path());
        }
    }
    if (pending.empty()) return;
    std::sort(pending.begin(), pending.end());

    for (const auto& p : pending) {
        std::vector<BYTE> data;
        if (!readBytesFromFile(p, data)) continue;
        std::string ext = p.extension().string();
        std::string mime = (ext == ".png" || ext == ".PNG") ? "image/png" : "image/jpeg";
        if (uploadBytesToDrive(auth, authFile, folderName, p.filename().string(), mime, data)) {
            fs::remove(p, ec);
            logMsg("Queue uploaded & deleted: " + p.filename().string());
            if (!quiet) {
                std::cout << "[Queue] Uploaded & deleted: " << p.filename().string() << "\n";
            }
        } else {
            break;  // Still offline; try again next cycle
        }
    }
}

class UploadWorker {
public:
    UploadWorker(AuthData& auth, fs::path authFile, fs::path queueDir, std::string folderName, bool quiet)
        : auth_(auth), authFile_(std::move(authFile)), queueDir_(std::move(queueDir)),
          folderName_(std::move(folderName)), quiet_(quiet) {
        for (int i = 0; i < 4; ++i) {
            threads_.emplace_back(&UploadWorker::run, this);
        }
    }

    ~UploadWorker() {
        stopAndJoin();
    }

    void enqueue(std::string name, std::string mime, std::vector<BYTE> data) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (queue_.size() >= 30) {
                std::error_code ec;
                fs::create_directories(queueDir_, ec);
                fs::path qPath = queueDir_ / name;
                if (writeBytesToFile(qPath, data) && !quiet_) {
                    std::cout << "[Offline/Full] Queued to disk: " << name << std::endl;
                }
                cv_.notify_one();
                return;
            }
            queue_.push_back(Item{std::move(name), std::move(mime), std::move(data)});
        }
        cv_.notify_one();
    }

    void stopAndJoin() {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (stopped_) return;
            stopRequested_ = true;
        }
        cv_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        std::lock_guard<std::mutex> lock(mtx_);
        stopped_ = true;
    }

    int uploadedCount() {
        std::lock_guard<std::mutex> lock(mtx_);
        return uploadedCount_;
    }

private:
    struct Item {
        std::string name;
        std::string mime;
        std::vector<BYTE> data;
    };

    bool claimNextPendingFile(fs::path& outPath) {
        std::error_code ec;
        if (!fs::exists(queueDir_, ec)) return false;

        auto now = std::chrono::steady_clock::now();
        if (now < offlineUntil_) return false;

        if (pendingCache_.empty()) {
            for (const auto& entry : fs::directory_iterator(queueDir_, ec)) {
                if (ec) break;
                if (entry.is_regular_file(ec)) {
                    fs::path p = entry.path();
                    if (inFlightFiles_.find(p) == inFlightFiles_.end()) {
                        pendingCache_.push_back(std::move(p));
                    }
                }
            }
            std::sort(pendingCache_.begin(), pendingCache_.end());
        }

        while (!pendingCache_.empty()) {
            fs::path p = std::move(pendingCache_.front());
            pendingCache_.pop_front();
            if (inFlightFiles_.find(p) == inFlightFiles_.end() && fs::exists(p, ec)) {
                inFlightFiles_.insert(p);
                outPath = std::move(p);
                return true;
            }
        }
        return false;
    }

    void run() {
        while (true) {
            fs::path pendingFile;
            Item item;
            bool hasPending = false;
            bool hasItem = false;
            bool shouldStop = false;

            {
                std::unique_lock<std::mutex> lock(mtx_);
                // Priority 1: Drain stored/queued files from pending_upload/ in parallel first
                if (claimNextPendingFile(pendingFile)) {
                    hasPending = true;
                } else if (!queue_.empty() && std::chrono::steady_clock::now() >= offlineUntil_) {
                    // Priority 2: Upload new live captures
                    item = std::move(queue_.front());
                    queue_.pop_front();
                    hasItem = true;
                } else if (!stopRequested_) {
                    cv_.wait_for(lock, std::chrono::seconds(2));
                    continue;
                } else {
                    if (!queue_.empty()) {
                        item = std::move(queue_.front());
                        queue_.pop_front();
                        hasItem = true;
                    } else {
                        shouldStop = true;
                    }
                }
            }

            if (hasPending) {
                try {
                    std::vector<BYTE> data;
                    if (readBytesFromFile(pendingFile, data)) {
                        std::string ext = pendingFile.extension().string();
                        std::string mime = (ext == ".png" || ext == ".PNG") ? "image/png" : "image/jpeg";
                        if (uploadBytesToDrive(auth_, authFile_, folderName_, pendingFile.filename().string(), mime, data)) {
                            std::error_code ec;
                            fs::remove(pendingFile, ec);
                            int cnt = 0;
                            {
                                std::lock_guard<std::mutex> lock(mtx_);
                                inFlightFiles_.erase(pendingFile);
                                cnt = ++uploadedCount_;
                            }
                            logMsg("Queue uploaded & deleted: " + pendingFile.filename().string());
                            if (!quiet_) {
                                std::cout << "[" << cnt << "] [Queue] Uploaded & deleted: "
                                          << pendingFile.filename().string() << std::endl;
                            }
                        } else {
                            std::lock_guard<std::mutex> lock(mtx_);
                            inFlightFiles_.erase(pendingFile);
                            offlineUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                        }
                    } else {
                        std::lock_guard<std::mutex> lock(mtx_);
                        inFlightFiles_.erase(pendingFile);
                    }
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> lock(mtx_);
                    inFlightFiles_.erase(pendingFile);
                    logMsg(std::string("Pending worker exception: ") + e.what());
                } catch (...) {
                    std::lock_guard<std::mutex> lock(mtx_);
                    inFlightFiles_.erase(pendingFile);
                }
                continue;
            }

            if (hasItem) {
                try {
                    if (uploadBytesToDrive(auth_, authFile_, folderName_, item.name, item.mime, item.data)) {
                        int cnt = 0;
                        {
                            std::lock_guard<std::mutex> lock(mtx_);
                            cnt = ++uploadedCount_;
                        }
                        logMsg("Uploaded: " + item.name);
                        if (!quiet_) {
                            std::cout << "[" << cnt << "] Uploaded to Drive: " << item.name
                                      << " (" << (item.data.size() / 1024) << " KB)" << std::endl;
                        }
                    } else {
                        {
                            std::lock_guard<std::mutex> lock(mtx_);
                            offlineUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                        }
                        std::error_code ec;
                        fs::create_directories(queueDir_, ec);
                        fs::path qPath = queueDir_ / item.name;
                        if (writeBytesToFile(qPath, item.data)) {
                            logMsg("Offline queued: " + item.name);
                            if (!quiet_) {
                                std::cout << "[Offline] Queued temporarily: " << item.name << std::endl;
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    logMsg(std::string("Worker exception: ") + e.what());
                } catch (...) {
                    logMsg("Worker unknown exception");
                }
                continue;
            }

            if (shouldStop) {
                break;
            }
        }
    }

    AuthData& auth_;
    fs::path authFile_;
    fs::path queueDir_;
    std::string folderName_;
    bool quiet_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    std::deque<fs::path> pendingCache_;
    std::set<fs::path> inFlightFiles_;
    std::chrono::steady_clock::time_point offlineUntil_{};
    bool stopRequested_ = false;
    bool stopped_ = false;
    int uploadedCount_ = 0;
    std::vector<std::thread> threads_;
};

void runInteractiveSetup(Options& opts, AuthData& auth, const fs::path& authFile, const fs::path& exeDir) {
    std::cout << "=== snap.exe Setup ===\n";

    std::cout << "Interval in seconds [default " << opts.intervalSeconds << "]: ";
    std::string line;
    std::getline(std::cin, line);
    if (!line.empty()) {
        try {
            double v = std::stod(line);
            if (v > 0) opts.intervalSeconds = v;
        } catch (...) {}
    }

    std::cout << "Min screen change %% to upload (0 = always) [default " << opts.diffThresholdPercent << "]: ";
    std::getline(std::cin, line);
    if (!line.empty()) {
        try {
            double v = std::stod(line);
            if (v >= 0 && v <= 100) opts.diffThresholdPercent = v;
        } catch (...) {}
    }

    std::cout << "Google Drive Folder ID (optional, press Enter for Drive root): ";
    std::getline(std::cin, line);
    if (!line.empty()) {
        opts.driveFolderId = line;
    }

    if (!loadAuthData(authFile, auth)) {
        std::cout << "\nGoogle Drive OAuth sign-in is not configured yet.\n";
        if (auth.clientId.empty()) auth.clientId = DEFAULT_CLIENT_ID;
        if (auth.clientSecret.empty()) auth.clientSecret = DEFAULT_CLIENT_SECRET;
        auth.driveFolderId = opts.driveFolderId;
        performOAuthLogin(auth, authFile);
    } else if (!opts.driveFolderId.empty()) {
        auth.driveFolderId = opts.driveFolderId;
        saveAuthData(authFile, auth);
    }

    std::cout << "Run automatically on Windows startup? (y/n) [default n]: ";
    std::getline(std::cin, line);
    if (!line.empty() && (line[0] == 'y' || line[0] == 'Y')) {
        std::cout << "Hide console window on startup? (y/n) [default y]: ";
        std::string bgLine;
        std::getline(std::cin, bgLine);
        opts.background = bgLine.empty() || bgLine[0] == 'y' || bgLine[0] == 'Y';
        installStartupEntry(opts, getExePath(), exeDir / "captures");
        opts.background = false;
    }

    std::cout << "Setup complete! Starting continuous capture...\n\n";
}

// MARK: - Main

int main(int argc, char* argv[]) {
    SetProcessDPIAware();
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    Options opts;
    if (!parseArgs(argc, argv, opts)) {
        CoUninitialize();
        return 1;
    }
    if (opts.showHelp) {
        printUsage();
        CoUninitialize();
        return 0;
    }
    if (opts.showVersion) {
        std::cout << SNAP_VERSION << "\n";
        CoUninitialize();
        return 0;
    }

    fs::path exePath = getExePath();
    fs::path exeDir = exePath.parent_path();
    g_logPath = exeDir / "snap.log";
    fs::path authFile = exeDir / "snap_auth.dat";
    fs::path queueDir = exeDir / "pending_upload";

    if (opts.startupStatus) {
        printStartupStatus();
        CoUninitialize();
        return 0;
    }
    if (opts.removeStartup) {
        bool ok = removeStartupEntry();
        CoUninitialize();
        return ok ? 0 : 1;
    }
    if (opts.doLogout) {
        std::error_code ec;
        fs::remove(authFile, ec);
        std::cout << "Logged out from Google Drive (removed snap_auth.dat).\n";
        CoUninitialize();
        return 0;
    }

    AuthData auth;
    if (opts.clientId.empty()) {
        loadCredentialsJsonIfPresent(exeDir / "credentials.json", auth);
    }
    if (!opts.clientId.empty()) auth.clientId = opts.clientId;
    if (!opts.clientSecret.empty()) auth.clientSecret = opts.clientSecret;
    if (auth.clientId.empty()) auth.clientId = DEFAULT_CLIENT_ID;
    if (auth.clientSecret.empty()) auth.clientSecret = DEFAULT_CLIENT_SECRET;
    auth.driveFolderId = opts.driveFolderId;

    if (opts.doLogin) {
        bool ok = performOAuthLogin(auth, authFile);
        CoUninitialize();
        return ok ? 0 : 1;
    }

    fs::path resolvedOutDir = opts.outputDir.empty()
        ? (exeDir / "captures")
        : fs::absolute(fs::path(opts.outputDir));

    if (!opts.localOnly && !opts.setup) {
        if (!loadAuthData(authFile, auth)) {
            std::cout << "First launch: Google Drive sign-in is required.\n";
            if (!performOAuthLogin(auth, authFile)) {
                CoUninitialize();
                return 1;
            }
        }
        if (!opts.driveFolderId.empty()) {
            auth.driveFolderId = opts.driveFolderId;
        }
    }

    if (opts.cleanDrive) {
        if (opts.localOnly) {
            std::cerr << "Error: --clean-drive cannot be used with --local.\n";
            CoUninitialize();
            return 1;
        }
        bool ok = cleanGoogleDrive(auth, authFile, opts.driveFolderName, opts.assumeYes);
        CoUninitialize();
        return ok ? 0 : 1;
    }

    if (opts.installStartup) {
        bool ok = installStartupEntry(opts, exePath, resolvedOutDir);
        CoUninitialize();
        return ok ? 0 : 1;
    }

    if (opts.setup) {
        runInteractiveSetup(opts, auth, authFile, exeDir);
        if (!opts.localOnly && !loadAuthData(authFile, auth)) {
            std::cerr << "Error: Google Drive sign-in is required.\n";
            CoUninitialize();
            return 1;
        }
    }

    if (opts.localOnly) {
        std::error_code ec;
        fs::create_directories(resolvedOutDir, ec);
        if (ec) {
            std::cerr << "Error creating output directory: " << ec.message() << "\n";
            CoUninitialize();
            return 1;
        }
    }

    bool rectOk = false;
    RECT captureRect = getCaptureRect(opts.displayNumber, rectOk);
    if (!rectOk) {
        CoUninitialize();
        return 1;
    }

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) != Gdiplus::Ok) {
        std::cerr << "Error initializing GDI+.\n";
        CoUninitialize();
        return 1;
    }

    CLSID encoderClsid{};
    bool isJpeg = (opts.format != "png");
    const WCHAR* mimeW = isJpeg ? L"image/jpeg" : L"image/png";
    std::string mimeA = isJpeg ? "image/jpeg" : "image/png";
    if (GetEncoderClsid(mimeW, &encoderClsid) < 0) {
        std::cerr << "Error finding image encoder.\n";
        Gdiplus::GdiplusShutdown(gdiplusToken);
        CoUninitialize();
        return 1;
    }

    int shotCount = 0;
    std::vector<BYTE> prevThumb;
    std::unique_ptr<UploadWorker> worker;
    if (!opts.localOnly && !opts.once) {
        worker = std::make_unique<UploadWorker>(auth, authFile, queueDir, opts.driveFolderName, opts.background);
    }

    auto processCapture = [&](bool forceUpload) {
        if (!opts.localOnly && !worker) {
            flushPendingQueue(queueDir, auth, authFile, opts.driveFolderName, opts.background);
        }

        if (!forceUpload && opts.diffThresholdPercent > 0.0 && !prevThumb.empty()) {
            std::vector<BYTE> probeThumb;
            if (!captureThumbOnly(captureRect, probeThumb)) {
                return;
            }
            double diffPct = computeDiffPercent(prevThumb, probeThumb);
            if (diffPct < opts.diffThresholdPercent) {
                return;  // No movement; skip full capture & JPEG encoding
            }
        }

        std::vector<BYTE> imgBytes;
        std::vector<BYTE> currThumb;
        if (!captureToMemoryAndThumb(captureRect, encoderClsid, opts.jpegQuality, isJpeg, imgBytes, currThumb)) {
            if (!opts.background) std::cerr << "Capture failed.\n";
            return;
        }
        prevThumb = std::move(currThumb);

        if (opts.localOnly) {
            std::string name = makeTimestampFilename(resolvedOutDir, opts.format);
            fs::path outPath = resolvedOutDir / name;
            if (writeBytesToFile(outPath, imgBytes)) {
                ++shotCount;
                if (!opts.background) {
                    std::cout << "[" << shotCount << "] Saved locally: " << outPath.string() << std::endl;
                }
            }
        } else if (worker) {
            std::string name = makeTimestampFilename("", opts.format);
            worker->enqueue(std::move(name), mimeA, std::move(imgBytes));
        } else {
            std::string name = makeTimestampFilename("", opts.format);
            if (uploadBytesToDrive(auth, authFile, opts.driveFolderName, name, mimeA, imgBytes)) {
                ++shotCount;
                if (!opts.background) {
                    std::cout << "[" << shotCount << "] Uploaded to Drive: " << name
                              << " (" << (imgBytes.size() / 1024) << " KB)" << std::endl;
                }
            } else {
                std::error_code ec;
                fs::create_directories(queueDir, ec);
                std::string qName = makeTimestampFilename(queueDir, opts.format);
                fs::path qPath = queueDir / qName;
                if (writeBytesToFile(qPath, imgBytes) && !opts.background) {
                    std::cout << "[Offline] Queued temporarily: " << qPath.filename().string() << std::endl;
                }
            }
        }
    };

    if (opts.once) {
        processCapture(true);
        Gdiplus::GdiplusShutdown(gdiplusToken);
        CoUninitialize();
        return 0;
    }

    DxgiDuplicator dxgi(opts.displayNumber);
    bool useDxgi = dxgi.init();

    if (opts.background) {
        HWND hwnd = GetConsoleWindow();
        if (hwnd != nullptr) ShowWindow(hwnd, SW_HIDE);
    } else {
        std::cout << (useDxgi ? "OS Event-Driven capture active (DXGI Desktop Duplication" : "Fallback capture active (GDI")
                  << ", diff >= " << opts.diffThresholdPercent << "%, " << opts.format << ")"
                  << (opts.localOnly ? " -> local" : " -> Google Drive") << "\n";
        std::cout << "Waiting for Windows screen-change events... Press SPACE/ENTER for manual shot, 'q' or Ctrl+C to stop.\n";
    }

    auto emitCapturedBytes = [&](std::vector<BYTE>& imgBytes) {
        if (opts.localOnly) {
            std::string name = makeTimestampFilename(resolvedOutDir, opts.format);
            fs::path outPath = resolvedOutDir / name;
            if (writeBytesToFile(outPath, imgBytes)) {
                ++shotCount;
                if (!opts.background) {
                    std::cout << "[" << shotCount << "] Saved locally: " << outPath.string() << std::endl;
                }
            }
        } else if (worker) {
            std::string name = makeTimestampFilename("", opts.format);
            worker->enqueue(std::move(name), mimeA, std::move(imgBytes));
        }
    };

    std::deque<std::vector<BYTE>> recentThumbs;
    auto isDuplicateOfRecent = [&](const std::vector<BYTE>& thumb) {
        if (opts.diffThresholdPercent <= 0.0) return false;
        for (const auto& r : recentThumbs) {
            if (computeDiffPercent(r, thumb) < opts.diffThresholdPercent) {
                return true;
            }
        }
        return false;
    };
    auto recordRecent = [&](const std::vector<BYTE>& thumb) {
        if (thumb.empty()) return;
        recentThumbs.push_back(thumb);
        if (recentThumbs.size() > 3) recentThumbs.pop_front();
    };

    // Take 1 initial shot at startup
    processCapture(true);
    recordRecent(prevThumb);
    auto lastCapture = std::chrono::steady_clock::now();

    bool hasPending = false;
    std::vector<BYTE> pendingBytes;
    std::vector<BYTE> pendingThumb;
    std::chrono::steady_clock::time_point firstMotionTime{};
    std::chrono::steady_clock::time_point lastMotionTime{};

    while (g_running) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - lastCapture).count();
        bool cooldownOk = (opts.intervalSeconds <= 0.0 || elapsed >= opts.intervalSeconds);

        if (useDxgi) {
            std::vector<BYTE> dxgiBytes;
            int res = dxgi.waitForEventAndCapture(
                static_cast<UINT>(opts.pollMs),
                false,
                opts.diffThresholdPercent,
                encoderClsid,
                opts.jpegQuality,
                isJpeg,
                prevThumb,
                dxgiBytes
            );
            if (res == 1) {
                auto tNow = std::chrono::steady_clock::now();
                if (!hasPending) {
                    firstMotionTime = tNow;
                    hasPending = true;
                }
                lastMotionTime = tNow;
                pendingBytes = std::move(dxgiBytes);
                pendingThumb = prevThumb;
            } else if (res == -1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            if (hasPending && cooldownOk) {
                auto tNow = std::chrono::steady_clock::now();
                auto sinceLast = std::chrono::duration_cast<std::chrono::milliseconds>(tNow - lastMotionTime).count();
                auto sinceFirst = std::chrono::duration_cast<std::chrono::milliseconds>(tNow - firstMotionTime).count();
                if (sinceLast >= 450 || sinceFirst >= 2000) {
                    if (!isDuplicateOfRecent(pendingThumb)) {
                        emitCapturedBytes(pendingBytes);
                        recordRecent(pendingThumb);
                        lastCapture = tNow;
                    }
                    hasPending = false;
                    pendingBytes.clear();
                    pendingThumb.clear();
                }
            }
        } else {
            if (cooldownOk) {
                processCapture(false);
                lastCapture = std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(opts.pollMs));
        }

        if (!opts.background && _kbhit()) {
            int ch = _getch();
            if (ch == 'q' || ch == 'Q') {
                break;
            } else if (ch == ' ' || ch == '\r' || ch == '\n') {
                hasPending = false;
                pendingBytes.clear();
                processCapture(true);
                recordRecent(prevThumb);
                lastCapture = std::chrono::steady_clock::now();
            }
        }
    }

    if (worker) {
        worker->stopAndJoin();
        shotCount = worker->uploadedCount();
    }
    if (!opts.background) {
        std::cout << "Done. Uploaded/captured " << shotCount << " screenshot(s).\n";
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    CoUninitialize();
    return 0;
}
