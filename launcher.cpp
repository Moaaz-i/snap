#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static const char* DEFAULT_GITHUB_REPO = "Moaaz-i/snap";
static const wchar_t* REG_RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* REG_VALUE_NAME = L"SnapLectureCapture";
static const wchar_t* MUTEX_NAME = L"Local\\SnapLectureLauncherMutex";

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
    if (len == 0) return fs::current_path() / "launcher.exe";
    return fs::path(std::wstring(buf, len));
}

std::wstring toWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring ws(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

void logMsg(const fs::path& logPath, const std::string& msg) {
    std::error_code ec;
    if (fs::exists(logPath, ec) && fs::file_size(logPath, ec) > 512 * 1024) {
        fs::remove(logPath, ec);
    }
    std::time_t now = std::time(nullptr);
    std::tm tmNow{};
    localtime_s(&tmNow, &now);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmNow);
    std::ofstream f(logPath, std::ios::app);
    if (f) {
        f << "[" << buf << "] " << msg << "\n";
    }
}

bool httpGetUrl(const std::string& fullUrl, std::vector<BYTE>& outBody, DWORD& outStatus) {
    outBody.clear();
    outStatus = 0;

    std::wstring wUrl = toWide(fullUrl);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256] = {0};
    wchar_t pathBuf[2048] = {0};
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = 2048;

    if (!WinHttpCrackUrl(wUrl.c_str(), static_cast<DWORD>(wUrl.size()), 0, &uc)) {
        return false;
    }

    HINTERNET hSession = WinHttpOpen(L"SnapLauncher/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 10000, 10000, 30000, 60000);

    HINTERNET hConnect = WinHttpConnect(hSession, uc.lpszHostName, uc.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", uc.lpszUrlPath, nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    std::wstring headers = L"Accept: application/vnd.github+json\r\nUser-Agent: SnapLauncher/1.0\r\n";
    BOOL sent = WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(headers.size()),
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

    bool ok = false;
    if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD statusSize = sizeof(outStatus);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &outStatus, &statusSize, WINHTTP_NO_HEADER_INDEX);
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            size_t oldSz = outBody.size();
            outBody.resize(oldSz + avail);
            DWORD readBytes = 0;
            if (WinHttpReadData(hRequest, outBody.data() + oldSz, avail, &readBytes) && readBytes > 0) {
                outBody.resize(oldSz + readBytes);
            } else {
                outBody.resize(oldSz);
                break;
            }
        }
        ok = (outStatus == 200);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok;
}

std::string jsonExtractStringAfter(const std::string& json, const std::string& key, size_t startPos = 0) {
    std::string pat = "\"" + key + "\"";
    size_t pos = json.find(pat, startPos);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    ++pos;
    std::string val;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '\\' && pos < json.size()) {
            val += json[pos++];
        } else if (c == '"') {
            break;
        } else {
            val += c;
        }
    }
    return val;
}

std::string findSnapExeDownloadUrl(const std::string& json) {
    size_t pos = 0;
    while (true) {
        size_t found = json.find("\"browser_download_url\"", pos);
        if (found == std::string::npos) break;
        std::string url = jsonExtractStringAfter(json, "browser_download_url", found);
        if (url.size() >= 8 && url.substr(url.size() - 8) == "snap.exe") {
            return url;
        }
        pos = found + 20;
    }
    return "";
}

std::string normalizeVersion(std::string v) {
    if (!v.empty() && (v[0] == 'v' || v[0] == 'V')) {
        v = v.substr(1);
    }
    while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) {
        v.pop_back();
    }
    return v;
}

std::string readInstalledVersion(const fs::path& exeDir) {
    fs::path verFile = exeDir / "installed_version.txt";
    std::ifstream f(verFile);
    if (f) {
        std::string v;
        std::getline(f, v);
        v = normalizeVersion(v);
        if (!v.empty()) return v;
    }
    return "0.0.0";
}

void writeInstalledVersion(const fs::path& exeDir, const std::string& v) {
    std::ofstream f(exeDir / "installed_version.txt", std::ios::trunc);
    if (f) f << v << "\n";
}

bool installStartupEntry(const fs::path& launcherPath, const std::string& repo) {
    HKEY hKey = nullptr;
    LONG res = RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_SET_VALUE, &hKey);
    if (res != ERROR_SUCCESS) return false;
    std::wstring cmd = L"\"" + launcherPath.wstring() + L"\" --repo \"" + toWide(repo) + L"\"";
    DWORD sz = static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t));
    res = RegSetValueExW(hKey, REG_VALUE_NAME, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), sz);
    RegCloseKey(hKey);
    if (res == ERROR_SUCCESS) {
        std::wcout << L"Startup enabled (Watchdog + Auto-Updater): " << cmd << L"\n";
        return true;
    }
    return false;
}

bool removeStartupEntry() {
    HKEY hKey = nullptr;
    LONG res = RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_SET_VALUE, &hKey);
    if (res != ERROR_SUCCESS) return false;
    res = RegDeleteValueW(hKey, REG_VALUE_NAME);
    RegCloseKey(hKey);
    std::cout << "Startup disabled.\n";
    return (res == ERROR_SUCCESS || res == ERROR_FILE_NOT_FOUND);
}

void printStartupStatus() {
    HKEY hKey = nullptr;
    LONG res = RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_QUERY_VALUE, &hKey);
    if (res != ERROR_SUCCESS) return;
    wchar_t buf[2048] = {0};
    DWORD sz = sizeof(buf), type = 0;
    res = RegQueryValueExW(hKey, REG_VALUE_NAME, nullptr, &type, reinterpret_cast<BYTE*>(buf), &sz);
    RegCloseKey(hKey);
    if (res == ERROR_SUCCESS && type == REG_SZ) {
        std::wcout << L"Startup: ENABLED\nCommand: " << buf << L"\n";
    } else {
        std::cout << "Startup: DISABLED\n";
    }
}

HANDLE startSnapProcess(const fs::path& snapExePath, const fs::path& exeDir) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + snapExePath.wstring() + L"\" --background";
    if (!CreateProcessW(
            snapExePath.wstring().c_str(),
            &cmd[0],
            nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            exeDir.wstring().c_str(),
            &si, &pi)) {
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

void stopSnapProcess(HANDLE& hProc) {
    if (!hProc) return;
    if (WaitForSingleObject(hProc, 0) == WAIT_TIMEOUT) {
        TerminateProcess(hProc, 0);
        WaitForSingleObject(hProc, 5000);
    }
    CloseHandle(hProc);
    hProc = nullptr;
}

void checkAndApplyUpdate(
    const std::string& repo,
    const fs::path& exeDir,
    const fs::path& snapExePath,
    const fs::path& logPath,
    HANDLE& childProc
) {
    std::string apiUrl = "https://api.github.com/repos/" + repo + "/releases/latest";
    std::vector<BYTE> body;
    DWORD status = 0;
    if (!httpGetUrl(apiUrl, body, status)) {
        return;  // Offline or no release published yet
    }

    std::string json(reinterpret_cast<const char*>(body.data()), body.size());
    std::string remoteTag = normalizeVersion(jsonExtractStringAfter(json, "tag_name"));
    std::string dlUrl = findSnapExeDownloadUrl(json);
    if (remoteTag.empty() || dlUrl.empty()) {
        return;
    }

    std::string localVer = readInstalledVersion(exeDir);
    if (remoteTag == localVer) {
        return;
    }

    logMsg(logPath, "New version available: " + remoteTag + " (current: " + localVer + "). Downloading...");
    std::vector<BYTE> exeBytes;
    DWORD dlStatus = 0;
    if (!httpGetUrl(dlUrl, exeBytes, dlStatus) || exeBytes.size() < 100 * 1024 ||
        exeBytes[0] != 'M' || exeBytes[1] != 'Z') {
        logMsg(logPath, "Update download failed or invalid binary (HTTP " + std::to_string(dlStatus) + ").");
        return;
    }

    fs::path newPath = exeDir / "snap.exe.new";
    fs::path bakPath = exeDir / "snap.exe.bak";
    {
        std::ofstream f(newPath, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(reinterpret_cast<const char*>(exeBytes.data()), static_cast<std::streamsize>(exeBytes.size()));
        if (!f.good()) return;
    }

    stopSnapProcess(childProc);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::error_code ec;
    fs::remove(bakPath, ec);
    if (fs::exists(snapExePath, ec)) {
        for (int attempt = 0; attempt < 6; ++attempt) {
            if (MoveFileExW(snapExePath.wstring().c_str(), bakPath.wstring().c_str(), MOVEFILE_REPLACE_EXISTING)) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    bool replaced = false;
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (MoveFileExW(newPath.wstring().c_str(), snapExePath.wstring().c_str(), MOVEFILE_REPLACE_EXISTING)) {
            replaced = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (!replaced) {
        logMsg(logPath, "Failed to replace snap.exe (error " + std::to_string(GetLastError()) + "). Rolling back.");
        if (fs::exists(bakPath, ec) && !fs::exists(snapExePath, ec)) {
            MoveFileExW(bakPath.wstring().c_str(), snapExePath.wstring().c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        fs::remove(newPath, ec);
        return;
    }

    writeInstalledVersion(exeDir, remoteTag);
    logMsg(logPath, "Updated snap.exe to version " + remoteTag + " successfully.");
    childProc = startSnapProcess(snapExePath, exeDir);
}

int main(int argc, char* argv[]) {
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    std::string repo = DEFAULT_GITHUB_REPO;
    int checkIntervalSec = 600;
    bool showConsole = false;
    bool installStartup = false;
    bool removeStartup = false;
    bool startupStatus = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--repo" && i + 1 < argc) {
            repo = argv[++i];
        } else if (arg == "--check-interval" && i + 1 < argc) {
            checkIntervalSec = std::max(30, std::atoi(argv[++i]));
        } else if (arg == "--console") {
            showConsole = true;
        } else if (arg == "--install-startup") {
            installStartup = true;
        } else if (arg == "--remove-startup") {
            removeStartup = true;
        } else if (arg == "--startup-status") {
            startupStatus = true;
        }
    }

    fs::path launcherPath = getExePath();
    fs::path exeDir = launcherPath.parent_path();
    fs::path snapExePath = exeDir / "snap.exe";
    fs::path logPath = exeDir / "launcher.log";

    if (startupStatus || removeStartup || installStartup) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            std::freopen("CONOUT$", "w", stdout);
            std::freopen("CONOUT$", "w", stderr);
        }
    }

    if (startupStatus) { printStartupStatus(); return 0; }
    if (removeStartup) { return removeStartupEntry() ? 0 : 1; }
    if (installStartup) { return installStartupEntry(launcherPath, repo) ? 0 : 1; }

    HANDLE hMutex = CreateMutexW(nullptr, TRUE, MUTEX_NAME);
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        return 0;  // Already running
    }
    (void)showConsole;

    logMsg(logPath, "Launcher started (repo: " + repo + ").");
    HANDLE childProc = nullptr;
    auto lastUpdateCheck = std::chrono::steady_clock::now() - std::chrono::seconds(checkIntervalSec);

    while (g_running) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastUpdateCheck).count() >= checkIntervalSec) {
            checkAndApplyUpdate(repo, exeDir, snapExePath, logPath, childProc);
            lastUpdateCheck = std::chrono::steady_clock::now();
        }

        bool alive = (childProc != nullptr && WaitForSingleObject(childProc, 0) == WAIT_TIMEOUT);
        if (!alive) {
            if (childProc) {
                CloseHandle(childProc);
                childProc = nullptr;
                logMsg(logPath, "snap.exe exited unexpectedly; restarting in 2s...");
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
            if (fs::exists(snapExePath)) {
                childProc = startSnapProcess(snapExePath, exeDir);
                if (childProc) {
                    logMsg(logPath, "Started snap.exe in background.");
                } else {
                    logMsg(logPath, "Failed to start snap.exe.");
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    stopSnapProcess(childProc);
    logMsg(logPath, "Launcher stopped.");
    if (hMutex) CloseHandle(hMutex);
    return 0;
}
