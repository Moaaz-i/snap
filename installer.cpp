#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace fs = std::filesystem;

#ifndef SNAP_VERSION
#define SNAP_VERSION "1.0.7"
#endif

bool runCmdWait(const std::wstring& cmdLine, bool hideWindow, DWORD& outExitCode) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    if (hideWindow) {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdLine;
    DWORD flags = hideWindow ? CREATE_NO_WINDOW : 0;
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &outExitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool startDetached(const fs::path& exePath, const fs::path& workDir) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exePath.wstring() + L"\"";
    if (!CreateProcessW(
            exePath.wstring().c_str(),
            &cmd[0],
            nullptr, nullptr, FALSE,
            DETACHED_PROCESS,
            nullptr,
            workDir.wstring().c_str(),
            &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool extractResource(int resId, const fs::path& destPath) {
    HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(resId), MAKEINTRESOURCEW(10));
    if (!hRes) return false;
    HGLOBAL hMem = LoadResource(nullptr, hRes);
    if (!hMem) return false;
    DWORD sz = SizeofResource(nullptr, hRes);
    const void* ptr = LockResource(hMem);
    if (!ptr || sz == 0) return false;

    for (int attempt = 0; attempt < 6; ++attempt) {
        std::ofstream f(destPath, std::ios::binary | std::ios::trunc);
        if (f) {
            f.write(static_cast<const char*>(ptr), static_cast<std::streamsize>(sz));
            if (f.good()) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return false;
}

int main() {
    std::cout << "============================================\n";
    std::cout << "  Snap Lecture Capture Installer (v" << SNAP_VERSION << ")\n";
    std::cout << "============================================\n\n";

    wchar_t* localAppData = nullptr;
    size_t len = 0;
    _wdupenv_s(&localAppData, &len, L"LOCALAPPDATA");
    if (!localAppData) {
        std::cerr << "[ERROR] Could not locate LOCALAPPDATA.\n";
        system("pause");
        return 1;
    }
    fs::path installDir = fs::path(localAppData) / L"SnapLectureCapture";
    free(localAppData);

    std::error_code ec;
    fs::create_directories(installDir, ec);
    if (ec) {
        std::cerr << "[ERROR] Failed to create install directory: " << ec.message() << "\n";
        system("pause");
        return 1;
    }

    fs::path snapPath = installDir / L"snap.exe";
    fs::path launcherPath = installDir / L"launcher.exe";

    std::cout << "[1/4] Stopping any running instances...\n";
    DWORD dummy = 0;
    runCmdWait(L"taskkill /IM launcher.exe /F", true, dummy);
    runCmdWait(L"taskkill /IM snap.exe /F", true, dummy);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    std::cout << "[2/4] Extracting snap.exe and launcher.exe to " << installDir.string() << "...\n";
    if (!extractResource(101, snapPath)) {
        std::cerr << "[ERROR] Failed to extract snap.exe (is it still running?).\n";
        system("pause");
        return 1;
    }
    if (!extractResource(102, launcherPath)) {
        std::cerr << "[ERROR] Failed to extract launcher.exe (is it still running?).\n";
        system("pause");
        return 1;
    }

    {
        std::ofstream vf(installDir / L"installed_version.txt", std::ios::trunc);
        if (vf) vf << SNAP_VERSION << "\n";
    }

    // Clear any cached RUNASADMIN compatibility flags
    HKEY hLayers = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers",
                      0, KEY_SET_VALUE, &hLayers) == ERROR_SUCCESS) {
        RegDeleteValueW(hLayers, snapPath.wstring().c_str());
        RegDeleteValueW(hLayers, launcherPath.wstring().c_str());
        RegCloseKey(hLayers);
    }

    std::cout << "[3/4] Checking Google Drive OAuth login...\n";
    if (!fs::exists(installDir / L"snap_auth.dat", ec)) {
        std::cout << "Opening browser for first-time Google Drive sign-in...\n";
        DWORD loginExit = 1;
        if (!runCmdWait(L"\"" + snapPath.wstring() + L"\" --login", false, loginExit) || loginExit != 0) {
            std::cerr << "[ERROR] Google Drive sign-in failed.\n";
            system("pause");
            return 1;
        }
    } else {
        std::cout << "Already signed in (snap_auth.dat found).\n";
    }

    std::cout << "[4/4] Enabling instant startup and starting background capture...\n";
    DWORD startExit = 1;
    runCmdWait(L"\"" + launcherPath.wstring() + L"\" --install-startup", false, startExit);
    if (!startDetached(launcherPath, installDir)) {
        std::cerr << "[ERROR] Failed to start launcher.exe.\n";
        system("pause");
        return 1;
    }

    std::cout << "\n============================================\n";
    std::cout << "  Done! Installed and running in background.\n";
    std::cout << "  - Folder: " << installDir.string() << "\n";
    std::cout << "  - Auto-starts instantly on every login\n";
    std::cout << "  - Auto-updates snap.exe from Moaaz-i/snap\n";
    std::cout << "============================================\n";
    system("pause");
    return 0;
}
