@echo off
setlocal

set "INSTALL_DIR=%LOCALAPPDATA%\SnapLectureCapture"
set "SNAP_SRC=%~dp0snap.exe"
set "LAUNCHER_SRC=%~dp0launcher.exe"
set "SNAP_DST=%INSTALL_DIR%\snap.exe"
set "LAUNCHER_DST=%INSTALL_DIR%\launcher.exe"

if not exist "%SNAP_SRC%" (
    echo [ERROR] snap.exe not found next to setup.bat!
    pause
    exit /b 1
)
if not exist "%LAUNCHER_SRC%" (
    echo [ERROR] launcher.exe not found next to setup.bat!
    pause
    exit /b 1
)

echo ============================================
echo   Snap Lecture Capture - Windows Setup
echo ============================================
echo.

if not exist "%INSTALL_DIR%" (
    mkdir "%INSTALL_DIR%"
)

echo [1/4] Stopping any running instances and copying files...
taskkill /IM launcher.exe /F >nul 2>&1
taskkill /IM snap.exe /F >nul 2>&1
timeout /t 1 /nobreak >nul

copy /Y "%SNAP_SRC%" "%SNAP_DST%" >nul
if errorlevel 1 (
    echo [ERROR] Failed to copy snap.exe.
    pause
    exit /b 1
)
copy /Y "%LAUNCHER_SRC%" "%LAUNCHER_DST%" >nul
if errorlevel 1 (
    echo [ERROR] Failed to copy launcher.exe.
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -Command "Unblock-File -Path '%INSTALL_DIR%\*' -ErrorAction SilentlyContinue" >nul 2>&1

for /f "delims=" %%v in ('"%SNAP_DST%" --version 2^>nul') do set "CUR_VER=%%v"
if not "%CUR_VER%"=="" (
    echo %CUR_VER%> "%INSTALL_DIR%\installed_version.txt"
)

echo [2/4] Checking Google Drive OAuth login...
if not exist "%INSTALL_DIR%\snap_auth.dat" (
    echo Opening browser for first-time Google Drive sign-in...
    "%SNAP_DST%" --login
    if errorlevel 1 (
        echo [ERROR] Google Drive sign-in failed.
        pause
        exit /b 1
    )
) else (
    echo Already signed in ^(snap_auth.dat found^).
)

echo [3/4] Enabling Watchdog + Auto-Updater on Windows startup...
reg delete "HKCU\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" /v "%LAUNCHER_DST%" /f >nul 2>&1
reg delete "HKCU\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers" /v "%SNAP_DST%" /f >nul 2>&1
"%LAUNCHER_DST%" --install-startup
if errorlevel 1 (
    echo [ERROR] Failed to register Windows Startup.
    pause
    exit /b 1
)

echo [4/4] Starting launcher.exe (Watchdog + Auto-Updater) in background...
start "" "%LAUNCHER_DST%"

echo.
echo ============================================
echo   Setup complete!
echo   - Installed at: %INSTALL_DIR%
echo   - Watchdog + Auto-Updater running in background
echo   - Checks GitHub (Moaaz-i/snap) for updates automatically
echo   - Restarts snap.exe automatically if it ever stops
echo ============================================
pause
