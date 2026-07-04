#pragma once

#include <string>
#include <windows.h>

namespace sts
{
enum class Theme
{
    Light,
    Dark
};

struct PlaylistConfig
{
    std::wstring lightPlaylist = L"white_auto";
    std::wstring darkPlaylist = L"black_auto";
    double latitude = 0.0;
    double longitude = 0.0;
    bool hasLocation = false;
};

class App
{
public:
    int run();
    Theme getSystemTheme();
    std::wstring detectWallpaperEnginePath();

private:
    bool acquireSingleInstance();
    void releaseSingleInstance();

    void applyTheme(Theme targetTheme, bool switchWallpaper = true);
    void repairAccentColor();
    void loop();

    // Config
    PlaylistConfig loadConfig();
    void saveConfig(const PlaylistConfig& cfg);
    void saveLocationToConfig(double lat, double lng);
    bool loadLocationFromConfig(double& lat, double& lng);

    // Service helpers
    static DWORD WINAPI serviceWorkerThread(LPVOID param);

    // Session-aware registry (handles Session 0 vs user session)
    bool writeUserDword(const wchar_t* key, const wchar_t* value, DWORD data);
    bool readUserDword(const wchar_t* key, const wchar_t* value, DWORD& out);

private:
    HANDLE mutex_ = nullptr;
    std::wstring weExePath_;
    PlaylistConfig config_;
    bool isService_ = false;
    std::wstring userSid_;

    // Service state
    SERVICE_STATUS_HANDLE serviceStatusHandle_ = nullptr;
    SERVICE_STATUS serviceStatus_ = {};
    friend VOID WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
    friend VOID WINAPI ServiceCtrlHandler(DWORD ctrl);
};

// Exposed for main.cpp
VOID WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
VOID WINAPI ServiceCtrlHandler(DWORD ctrl);

} // namespace sts
