#include "Solune.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <tlhelp32.h>
#include <unordered_set>
#include <vector>
#include <wtsapi32.h>

#include <userenv.h>

#include <winhttp.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Wtsapi32.lib")
#pragma comment(lib, "Userenv.lib")
#pragma comment(lib, "Winhttp.lib")
#pragma comment(lib, "windowsapp.lib")

namespace fs = std::filesystem;
using namespace winrt::Windows::Data::Json;

namespace sts
{

// ============================================================================
//  Static service pointer (set by ServiceMain, used by worker thread)
// ============================================================================
static App* g_pServiceApp = nullptr;

// ============================================================================
//  Utilities
// ============================================================================
static bool fileExists(const std::wstring& path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring getExeDir()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    const auto pos = s.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
        s.resize(pos);
    return s;
}

// ============================================================================
//  JSON helpers (lightweight, no WeConfigManager dependency)
// ============================================================================
static bool jsonTryGetString(JsonObject const& obj, const std::wstring& key, std::wstring& out)
{
    if (!obj.HasKey(key))
        return false;
    auto val = obj.GetNamedValue(key);
    if (val.ValueType() != JsonValueType::String)
        return false;
    out = val.GetString().c_str();
    return true;
}

static bool jsonTryGetNumber(JsonObject const& obj, const std::wstring& key, double& out)
{
    if (!obj.HasKey(key))
        return false;
    auto val = obj.GetNamedValue(key);
    if (val.ValueType() != JsonValueType::Number)
        return false;
    out = val.GetNumber();
    return true;
}

static bool jsonTryGetObject(JsonObject const& obj, const std::wstring& key, JsonObject& out)
{
    if (!obj.HasKey(key))
        return false;
    auto val = obj.GetNamedValue(key);
    if (val.ValueType() != JsonValueType::Object)
        return false;
    out = val.GetObject();
    return true;
}

// ============================================================================
//  Registry
// ============================================================================
namespace registry
{
static bool readDword(HKEY root, const wchar_t* subKey, const wchar_t* valueName, DWORD& out)
{
    HKEY hKey{};
    if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, cb = sizeof(DWORD), value = 0;
    const LONG rc = RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<BYTE*>(&value), &cb);
    RegCloseKey(hKey);
    if (rc == ERROR_SUCCESS && type == REG_DWORD) { out = value; return true; }
    return false;
}

static bool writeDword(HKEY root, const wchar_t* subKey, const wchar_t* valueName, DWORD value)
{
    HKEY hKey{};
    if (RegCreateKeyExW(root, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
        return false;
    const LONG rc = RegSetValueExW(hKey, valueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(hKey);
    return rc == ERROR_SUCCESS;
}
} // namespace registry

// ============================================================================
//  Config (solune.json)
// ============================================================================
static std::wstring configPath()
{
    return getExeDir() + L"\\solune.json";
}

PlaylistConfig App::loadConfig()
{
    PlaylistConfig cfg;

    const std::wstring path = configPath();
    if (!fileExists(path))
    {
        saveConfig(cfg);
        return cfg;
    }

    try
    {
        std::ifstream file(fs::path(path), std::ios::binary);
        if (!file.is_open())
            return cfg;

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        JsonObject root;
        if (!JsonObject::TryParse(winrt::to_hstring(content), root))
            return cfg;

        jsonTryGetString(root, L"light_playlist", cfg.lightPlaylist);
        jsonTryGetString(root, L"dark_playlist", cfg.darkPlaylist);

        JsonObject loc;
        if (jsonTryGetObject(root, L"location", loc))
        {
            if (jsonTryGetNumber(loc, L"lat", cfg.latitude) &&
                jsonTryGetNumber(loc, L"lng", cfg.longitude) &&
                (std::abs(cfg.latitude) > 0.01 || std::abs(cfg.longitude) > 0.01))
                cfg.hasLocation = true;
        }
    }
    catch (...) {}

    return cfg;
}

void App::saveConfig(const PlaylistConfig& cfg)
{
    try
    {
        JsonObject root;
        root.SetNamedValue(L"light_playlist", JsonValue::CreateStringValue(cfg.lightPlaylist));
        root.SetNamedValue(L"dark_playlist", JsonValue::CreateStringValue(cfg.darkPlaylist));

        if (cfg.hasLocation)
        {
            JsonObject loc;
            loc.SetNamedValue(L"lat", JsonValue::CreateNumberValue(cfg.latitude));
            loc.SetNamedValue(L"lng", JsonValue::CreateNumberValue(cfg.longitude));
            root.SetNamedValue(L"location", loc);
        }

        const std::wstring path = configPath();
        const std::string json = winrt::to_string(root.Stringify());
        std::ofstream file(fs::path(path), std::ios::binary | std::ios::trunc);
        file.write(json.c_str(), json.size());
    }
    catch (...) {}
}

void App::saveLocationToConfig(double lat, double lng)
{
    PlaylistConfig cfg = loadConfig();
    cfg.latitude = lat;
    cfg.longitude = lng;
    cfg.hasLocation = true;
    saveConfig(cfg);
}

bool App::loadLocationFromConfig(double& lat, double& lng)
{
    PlaylistConfig cfg = loadConfig();
    if (cfg.hasLocation)
    {
        lat = cfg.latitude;
        lng = cfg.longitude;
        return true;
    }
    return false;
}

// ============================================================================
//  Sun calculation (localSun namespace)
// ============================================================================
namespace localSun
{
struct Location { double latitude = 0.0; double longitude = 0.0; bool valid = false; };

struct SunWindow
{
    long long sunriseSec = -1;
    long long sunsetSec = -1;
    bool valid = false;
};

static constexpr double kPi = 3.141592653589793;
static constexpr double kRadPerDeg = kPi / 180.0;
static constexpr double kDegPerRad = 180.0 / kPi;

static double degToRad(double x) { return x * kRadPerDeg; }
static double radToDeg(double x) { return x * kDegPerRad; }

static Location getLocationViaHttp()
{
    Location loc;
    // ip-api.com free tier: 45 req/min, no API key needed
    const wchar_t* host = L"ip-api.com";
    const wchar_t* path = L"/json/";

    HINTERNET session = WinHttpOpen(L"Solune/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return loc;

    HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTP_PORT, 0);
    if (!connect) { WinHttpCloseHandle(session); return loc; }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            0);
    if (!request) { WinHttpCloseHandle(connect); WinHttpCloseHandle(session); return loc; }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
    {
        WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session);
        return loc;
    }

    if (!WinHttpReceiveResponse(request, nullptr))
    {
        WinHttpCloseHandle(request); WinHttpCloseHandle(connect); WinHttpCloseHandle(session);
        return loc;
    }

    std::string body;
    DWORD bytesAvail = 0;
    while (WinHttpQueryDataAvailable(request, &bytesAvail) && bytesAvail > 0)
    {
        std::vector<char> chunk(bytesAvail + 1);
        DWORD read = 0;
        if (WinHttpReadData(request, chunk.data(), bytesAvail, &read) && read > 0)
            body.append(chunk.data(), read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (body.empty()) return loc;

    try
    {
        auto json = winrt::to_hstring(body);
        JsonObject root;
        if (!JsonObject::TryParse(json, root)) return loc;

        double lat = 0.0, lng = 0.0;
        if (jsonTryGetNumber(root, L"lat", lat) &&
            jsonTryGetNumber(root, L"lon", lng) &&
            (std::abs(lat) > 0.01 || std::abs(lng) > 0.01))
        {
            loc.latitude = lat;
            loc.longitude = lng;
            loc.valid = true;
        }
    }
    catch (...) {}

    return loc;
}

static long long roundHoursToSeconds(double hours)
{
    long long seconds = static_cast<long long>(hours * 3600.0 + 0.5);
    seconds %= 86400;
    if (seconds < 0) seconds += 86400;
    return seconds;
}

static bool getSunWindowForToday(SunWindow& out, double lat, double lng)
{
    static double cachedLat = -999.0;
    static double cachedLng = -999.0;
    static int cachedDayOfYear = -1;
    static SunWindow cachedWindow{};

    SYSTEMTIME lt{};
    GetLocalTime(&lt);
    const int dayOfYear = lt.wYear * 366 + lt.wMonth * 31 + lt.wDay;

    if (cachedWindow.valid && cachedDayOfYear == dayOfYear &&
        std::abs(cachedLat - lat) < 0.001 && std::abs(cachedLng - lng) < 0.001)
    {
        out = cachedWindow;
        return true;
    }

    // Fallback: when location is unavailable, use system time: 7:00-19:00
    if (std::abs(lat) < 0.01 && std::abs(lng) < 0.01)
    {
        cachedWindow.sunriseSec = 7 * 3600;  // 07:00
        cachedWindow.sunsetSec  = 19 * 3600; // 19:00
        cachedWindow.valid = true;
        cachedDayOfYear = dayOfYear;
        cachedLat = lat;
        cachedLng = lng;
        out = cachedWindow;
        return true;
    }

    const int year = static_cast<int>(lt.wYear);
    const int month = static_cast<int>(lt.wMonth);
    const int day = static_cast<int>(lt.wDay);
    const int n1 = static_cast<int>((275 * month) / 9);
    const int n2 = static_cast<int>((month + 9) / 12);
    const int n3 = 1 + static_cast<int>((year - ((year >> 2) << 2) + 2) / 3);
    const int N = n1 - (n2 * n3) + day - 30;
    const double lngHour = lng / 15.0;

    auto calcLocalHours = [&](bool sunrise) -> double
    {
        const double t = N + ((sunrise ? 6.0 : 18.0) - lngHour) / 24.0;
        const double M = (0.9856 * t) - 3.289;
        double L = M + (1.916 * std::sin(degToRad(M))) + (0.020 * std::sin(degToRad(M + M))) + 282.634;
        while (L < 0.0) L += 360.0;
        while (L >= 360.0) L -= 360.0;
        double RA = radToDeg(std::atan(0.91764 * std::tan(degToRad(L))));
        while (RA < 0.0) RA += 360.0;
        while (RA >= 360.0) RA -= 360.0;
        const double Lquadrant = std::floor(L / 90.0) * 90.0;
        const double RAquadrant = std::floor(RA / 90.0) * 90.0;
        RA = (RA + (Lquadrant - RAquadrant)) / 15.0;
        const double sinDec = 0.39782 * std::sin(degToRad(L));
        const double cosDec = std::cos(std::asin(sinDec));
        const double cosH = (std::cos(degToRad(90.833)) - (sinDec * std::sin(degToRad(lat)))) /
                            (cosDec * std::cos(degToRad(lat)));
        if (cosH > 1.0 || cosH < -1.0) return -1.0;
        double H = sunrise ? (360.0 - radToDeg(std::acos(cosH))) : radToDeg(std::acos(cosH));
        H /= 15.0;
        const double T = H + RA - (0.06571 * t) - 6.622;
        double UT = T - lngHour;
        while (UT < 0.0) UT += 24.0;
        while (UT >= 24.0) UT -= 24.0;
        TIME_ZONE_INFORMATION tzi{};
        GetTimeZoneInformation(&tzi);
        double local = UT - (tzi.Bias / 60.0);
        while (local < 0.0) local += 24.0;
        while (local >= 24.0) local -= 24.0;
        return local;
    };

    const double riseHours = calcLocalHours(true);
    const double setHours = calcLocalHours(false);
    if (riseHours < 0.0 || setHours < 0.0)
    {
        cachedWindow = {};
        out = cachedWindow;
        return false;
    }

    cachedWindow.sunriseSec = roundHoursToSeconds(riseHours);
    cachedWindow.sunsetSec = roundHoursToSeconds(setHours);
    cachedWindow.valid = true;
    cachedDayOfYear = dayOfYear;
    cachedLat = lat;
    cachedLng = lng;
    out = cachedWindow;
    return true;
}

static Theme expectedThemeNow(double lat, double lng)
{
    SunWindow window;
    if (!getSunWindowForToday(window, lat, lng) || !window.valid)
        return Theme::Dark;
    SYSTEMTIME lt{};
    GetLocalTime(&lt);
    const long long nowSec = lt.wHour * 3600LL + lt.wMinute * 60LL + lt.wSecond;
    return (nowSec >= window.sunriseSec && nowSec < window.sunsetSec) ? Theme::Light : Theme::Dark;
}

static long long secondsUntilNextEvent(double lat, double lng)
{
    SunWindow window;
    if (!getSunWindowForToday(window, lat, lng) || !window.valid)
        return 60;

    SYSTEMTIME lt{};
    GetLocalTime(&lt);
    const long long nowSec = lt.wHour * 3600LL + lt.wMinute * 60LL + lt.wSecond;
    const long long daySec = 86400LL;

    // Check if we're before sunrise today
    if (nowSec < window.sunriseSec)
        return window.sunriseSec - nowSec;
    // Between sunrise and sunset
    if (nowSec < window.sunsetSec)
        return window.sunsetSec - nowSec;
    // After sunset: next event is tomorrow's sunrise
    return (daySec - nowSec) + window.sunriseSec;
}
} // namespace localSun

// ============================================================================
//  Theme refresh & accent repair
// ============================================================================
static void broadcastThemeRefresh()
{
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet"),
                        SMTO_ABORTIFHUNG, 2000, &result);
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"),
                        SMTO_ABORTIFHUNG, 2000, &result);
    SendMessageTimeoutW(HWND_BROADCAST, WM_THEMECHANGED, 0, 0, SMTO_ABORTIFHUNG, 2000, &result);

    auto refreshWindow = [](HWND hwnd) {
        if (!hwnd) return;
        InvalidateRect(hwnd, nullptr, TRUE);
        UpdateWindow(hwnd);
        PostMessageW(hwnd, WM_THEMECHANGED, 0, 0);
    };
    refreshWindow(FindWindowW(L"Shell_TrayWnd", nullptr));
    HWND sec = FindWindowW(L"Shell_SecondaryTrayWnd", nullptr);
    while (sec) { refreshWindow(sec); sec = FindWindowExW(nullptr, sec, L"Shell_SecondaryTrayWnd", nullptr); }
}

static void repairAccentColorAfterWallpaperSwitch()
{
    static constexpr const wchar_t* kKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    static constexpr const wchar_t* kDwm = L"Software\\Microsoft\\Windows\\DWM";
    static constexpr const wchar_t* kDesktop = L"Control Panel\\Desktop";

    DWORD taskbarAccent = 0, titleBarAccent = 0, autoColorization = 0;
    registry::readDword(HKEY_CURRENT_USER, kKey, L"ColorPrevalence", taskbarAccent);
    registry::readDword(HKEY_CURRENT_USER, kDwm, L"ColorPrevalence", titleBarAccent);
    registry::readDword(HKEY_CURRENT_USER, kDesktop, L"AutoColorization", autoColorization);

    broadcastThemeRefresh();
    Sleep(120);

    if (autoColorization == 1)
    {
        registry::writeDword(HKEY_CURRENT_USER, kDesktop, L"AutoColorization", 0);
        broadcastThemeRefresh();
        Sleep(120);
        registry::writeDword(HKEY_CURRENT_USER, kDesktop, L"AutoColorization", 1);
        Sleep(120);
    }

    registry::writeDword(HKEY_CURRENT_USER, kKey, L"ColorPrevalence", taskbarAccent);
    registry::writeDword(HKEY_CURRENT_USER, kDwm, L"ColorPrevalence", titleBarAccent);
    broadcastThemeRefresh();
}

// ============================================================================
//  User session helpers (for Service in Session 0)
// ============================================================================
static DWORD getActiveUserSessionId()
{
    DWORD sessionId = 0xFFFFFFFF;
    PWTS_SESSION_INFOA sessions = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsA(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count))
        return sessionId;

    for (DWORD i = 0; i < count; ++i)
    {
        if (sessions[i].State == WTSActive)
        {
            sessionId = sessions[i].SessionId;
            break;
        }
    }
    WTSFreeMemory(sessions);
    return sessionId;
}

static HANDLE getActiveUserToken()
{
    const DWORD sessionId = getActiveUserSessionId();
    if (sessionId == 0xFFFFFFFF)
        return nullptr;

    HANDLE userToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &userToken))
        return nullptr;

    // Duplicate to primary token for CreateProcessAsUser
    HANDLE primaryToken = nullptr;
    DuplicateTokenEx(userToken, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &primaryToken);
    CloseHandle(userToken);
    return primaryToken;
}

// ============================================================================
//  Theme application (works from both user session and Session 0)
// ============================================================================
static bool launchWallpaperEngineAsUser(const std::wstring& weExePath, const std::wstring& playlistName)
{
    if (weExePath.empty() || playlistName.empty())
        return false;

    std::wstring cmd = L"\"" + weExePath + L"\" -control openPlaylist -playlist \"" + playlistName + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.size() + 1);
    wcscpy_s(cmdBuf.data(), cmdBuf.size(), cmd.c_str());

    HANDLE userToken = getActiveUserToken();
    if (!userToken)
    {
        // Fallback: try normal CreateProcess (in user session mode)
        STARTUPINFOW si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return true;
        }
        return false;
    }

    // CreateProcessAsUser for Session 0 -> user session
    LPVOID envBlock = nullptr;
    CreateEnvironmentBlock(&envBlock, userToken, FALSE);

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    const BOOL ok = CreateProcessAsUserW(userToken, nullptr, cmdBuf.data(), nullptr, nullptr,
                                         FALSE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                         envBlock, nullptr, &si, &pi);

    if (envBlock) DestroyEnvironmentBlock(envBlock);
    CloseHandle(userToken);

    if (ok)
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
    }
    return false;
}

void App::applyTheme(Theme targetTheme, bool switchWallpaper)
{
    static constexpr const wchar_t* kPersonalizeKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    static constexpr const wchar_t* kDwmKey = L"Software\\Microsoft\\Windows\\DWM";

    const DWORD themeValue = (targetTheme == Theme::Dark) ? 0u : 1u;
    const DWORD accentVisible = (targetTheme == Theme::Dark) ? 1u : 0u;

    registry::writeDword(HKEY_CURRENT_USER, kPersonalizeKey, L"SystemUsesLightTheme", themeValue);
    registry::writeDword(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme", themeValue);
    registry::writeDword(HKEY_CURRENT_USER, kPersonalizeKey, L"ColorPrevalence", accentVisible);
    registry::writeDword(HKEY_CURRENT_USER, kDwmKey, L"ColorPrevalence", accentVisible);

    broadcastThemeRefresh();

    if (switchWallpaper && !weExePath_.empty())
    {
        const std::wstring& playlistName = (targetTheme == Theme::Dark)
            ? config_.darkPlaylist : config_.lightPlaylist;
        launchWallpaperEngineAsUser(weExePath_, playlistName);
        Sleep(1500);
    }

    repairAccentColorAfterWallpaperSwitch();
}

// ============================================================================
//  Wallpaper Engine detection
// ============================================================================
static std::wstring normalizeWindowsPath(std::wstring path)
{
    std::replace(path.begin(), path.end(), L'/', L'\\');
    std::wstring out;
    out.reserve(path.size());
    wchar_t prev = 0;
    for (size_t i = 0; i < path.size(); ++i)
    {
        const wchar_t ch = path[i];
        if (ch == L'\\' && prev == L'\\' && !(i == 1 && !out.empty() && out.front() == L'\\'))
            continue;
        out.push_back(ch);
        prev = ch;
    }
    return out;
}

static bool readRegistryStringW(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                                 std::wstring& out, REGSAM wowFlags = 0)
{
    HKEY hKey{};
    if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE | wowFlags, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, bytes = 0;
    LONG rc = RegQueryValueExW(hKey, valueName, nullptr, &type, nullptr, &bytes);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0) { RegCloseKey(hKey); return false; }
    std::wstring buffer((bytes + sizeof(wchar_t) - 1) / sizeof(wchar_t), L'\0');
    rc = RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()), &bytes);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS) return false;
    while (!buffer.empty() && buffer.back() == L'\0') buffer.pop_back();
    if (type == REG_EXPAND_SZ)
    {
        const DWORD needed = ExpandEnvironmentStringsW(buffer.c_str(), nullptr, 0);
        if (needed > 0)
        {
            std::wstring expanded(needed, L'\0');
            ExpandEnvironmentStringsW(buffer.c_str(), expanded.data(), needed);
            while (!expanded.empty() && expanded.back() == L'\0') expanded.pop_back();
            out = std::move(expanded);
            return true;
        }
    }
    out = std::move(buffer);
    return !out.empty();
}

static std::wstring unescapeVdfPath(std::string value)
{
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '\\') { out.push_back('\\'); ++i; }
        else out.push_back(value[i]);
    }
    // UTF-8 to wide
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, out.c_str(), static_cast<int>(out.size()), nullptr, 0);
    std::wstring wide(wideLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, out.c_str(), static_cast<int>(out.size()), wide.data(), wideLen);
    return normalizeWindowsPath(std::move(wide));
}

static void appendUniquePath(std::vector<std::wstring>& paths, std::unordered_set<std::wstring>& seen, std::wstring path)
{
    path = normalizeWindowsPath(std::move(path));
    if (path.empty()) return;
    if (seen.insert(path).second) paths.push_back(std::move(path));
}

static std::wstring getEnvString(const wchar_t* name)
{
    wchar_t* buffer = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&buffer, &len, name) != 0 || !buffer || len == 0) { free(buffer); return {}; }
    const std::wstring value(buffer);
    free(buffer);
    return value;
}

static std::wstring findProcessImagePath(const wchar_t* exeName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe)) { CloseHandle(snap); return {}; }
    do
    {
        if (_wcsicmp(pe.szExeFile, exeName) != 0) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!process) continue;
        std::wstring path(32768, L'\0');
        DWORD size = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(process, 0, path.data(), &size))
        {
            path.resize(size);
            CloseHandle(process);
            CloseHandle(snap);
            return normalizeWindowsPath(std::move(path));
        }
        CloseHandle(process);
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    return {};
}

static void appendSteamInstallCandidates(std::vector<std::wstring>& roots)
{
    std::unordered_set<std::wstring> seen;
    for (const auto& p : roots) { if (!p.empty()) seen.insert(normalizeWindowsPath(p)); }

    std::wstring value;
    if (readRegistryStringW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", value))
        appendUniquePath(roots, seen, value);
    if (readRegistryStringW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"InstallPath", value))
        appendUniquePath(roots, seen, value);
    if (readRegistryStringW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", value, KEY_WOW64_64KEY))
        appendUniquePath(roots, seen, value);
    if (readRegistryStringW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", value, KEY_WOW64_32KEY))
        appendUniquePath(roots, seen, value);
    if (readRegistryStringW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath", value))
        appendUniquePath(roots, seen, value);

    const std::wstring pf86 = getEnvString(L"ProgramFiles(x86)");
    const std::wstring pf = getEnvString(L"ProgramFiles");
    const std::wstring localAppData = getEnvString(L"LOCALAPPDATA");
    if (!pf86.empty()) appendUniquePath(roots, seen, pf86 + L"\\Steam");
    if (!pf.empty()) appendUniquePath(roots, seen, pf + L"\\Steam");
    if (!localAppData.empty()) appendUniquePath(roots, seen, localAppData + L"\\Programs\\Steam");
}

static std::vector<std::wstring> parseSteamLibraries(const std::wstring& steamPath)
{
    std::vector<std::wstring> libraries;
    std::unordered_set<std::wstring> seen;
    appendUniquePath(libraries, seen, steamPath);
    const std::wstring vdfPath = normalizeWindowsPath(steamPath) + L"\\steamapps\\libraryfolders.vdf";
    std::ifstream file(fs::path(vdfPath), std::ios::binary);
    if (!file.is_open()) return libraries;
    std::string line;
    while (std::getline(file, line))
    {
        const auto pathTag = line.find("\"path\"");
        if (pathTag == std::string::npos) continue;
        const auto firstQuote = line.find('"', pathTag + 6);
        if (firstQuote == std::string::npos) continue;
        const auto secondQuote = line.find('"', firstQuote + 1);
        if (secondQuote == std::string::npos || secondQuote <= firstQuote + 1) continue;
        appendUniquePath(libraries, seen,
                         unescapeVdfPath(line.substr(firstQuote + 1, secondQuote - firstQuote - 1)));
    }
    return libraries;
}

std::wstring App::detectWallpaperEnginePath()
{
    const std::wstring runningPath = findProcessImagePath(L"wallpaper64.exe");
    if (!runningPath.empty() && fileExists(runningPath)) return runningPath;

    std::vector<std::wstring> steamRoots;
    appendSteamInstallCandidates(steamRoots);
    std::unordered_set<std::wstring> seenLibraries;
    std::vector<std::wstring> libraries;
    for (const auto& root : steamRoots)
    {
        for (const auto& lib : parseSteamLibraries(root))
            appendUniquePath(libraries, seenLibraries, lib);
    }

    static const wchar_t* kExeNames[] = { L"wallpaper64.exe", L"wallpaper32.exe" };
    for (const auto& lib : libraries)
    {
        for (const wchar_t* exeName : kExeNames)
        {
            const std::wstring commonPath = lib + L"\\steamapps\\common\\wallpaper_engine\\" + exeName;
            if (fileExists(commonPath)) return commonPath;
            const std::wstring directPath = lib + L"\\" + exeName;
            if (fileExists(directPath)) return directPath;
        }
    }
    return {};
}

Theme App::getSystemTheme()
{
    DWORD systemLight = 1;
    if (registry::readDword(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme", systemLight) && systemLight == 1)
        return Theme::Light;
    return Theme::Dark;
}

// ============================================================================
//  Single instance
// ============================================================================
bool App::acquireSingleInstance()
{
    mutex_ = CreateMutexA(nullptr, TRUE, "Global\\SoluneMutex");
    if (!mutex_ || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (mutex_) CloseHandle(mutex_);
        mutex_ = nullptr;
        return false;
    }
    return true;
}

void App::releaseSingleInstance()
{
    if (mutex_) { ReleaseMutex(mutex_); CloseHandle(mutex_); mutex_ = nullptr; }
}

// ============================================================================
//  Windows Service
// ============================================================================
VOID WINAPI ServiceCtrlHandler(DWORD ctrl)
{
    App* app = g_pServiceApp;
    if (!app || !app->serviceStatusHandle_) return;

    switch (ctrl)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        app->serviceStatus_.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(app->serviceStatusHandle_, &app->serviceStatus_);
        break;
    case SERVICE_CONTROL_INTERROGATE:
        SetServiceStatus(app->serviceStatusHandle_, &app->serviceStatus_);
        break;
    default:
        break;
    }
}

// Forward declaration needed before ServiceMain
DWORD WINAPI App::serviceWorkerThread(LPVOID param)
{
    App* app = static_cast<App*>(param);
    g_pServiceApp = app;

    // Load config
    app->config_ = app->loadConfig();
    std::wcout << L"[Service] Solune service started." << std::endl;
    std::wcout << L"[Service] Light playlist: " << app->config_.lightPlaylist << std::endl;
    std::wcout << L"[Service] Dark playlist:  " << app->config_.darkPlaylist << std::endl;

    // Detect WE
    app->weExePath_ = app->detectWallpaperEnginePath();
    if (!app->weExePath_.empty())
        std::wcout << L"[Service] Wallpaper Engine detected: " << app->weExePath_ << std::endl;
    else
        std::wcout << L"[Service] Wallpaper Engine not found, system-theme-only mode." << std::endl;

    // Get location from cache or config
    double lat = 0.0, lng = 0.0;
    bool hasLocation = app->loadLocationFromConfig(lat, lng);
    if (hasLocation)
        std::wcout << L"[Service] Cached location: " << lat << L", " << lng << std::endl;
    else
        std::wcout << L"[Service] No cached location. Run Solune.exe once in console mode first." << std::endl;

    // Report running
    app->serviceStatus_.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(app->serviceStatusHandle_, &app->serviceStatus_);

    // Apply initial theme
    if (hasLocation)
    {
        Theme expected = localSun::expectedThemeNow(lat, lng);
        app->applyTheme(expected, true);
    }

    // Main service loop: check every 60 seconds
    int lastDayOfYear = -1;
    long long sunriseSec = -1, sunsetSec = -1;
    Theme lastApplied = Theme::Dark;

    while (app->serviceStatus_.dwCurrentState == SERVICE_RUNNING)
    {
        if (!hasLocation)
        {
            // Try to reload config in case location was updated
            hasLocation = app->loadLocationFromConfig(lat, lng);
            if (hasLocation)
                std::wcout << L"[Service] Location cache now available." << std::endl;
        }

        if (hasLocation)
        {
            SYSTEMTIME lt{};
            GetLocalTime(&lt);
            const int dayOfYear = lt.wYear * 366 + lt.wMonth * 31 + lt.wDay;

            // Recalculate sun window daily or on first run
            if (dayOfYear != lastDayOfYear)
            {
                localSun::SunWindow window;
                if (localSun::getSunWindowForToday(window, lat, lng))
                {
                    sunriseSec = window.sunriseSec;
                    sunsetSec = window.sunsetSec;
                    lastDayOfYear = dayOfYear;
                }
            }

            const long long nowSec = lt.wHour * 3600LL + lt.wMinute * 60LL + lt.wSecond;

            // Check if we crossed sunrise (within last minute)
            if (sunriseSec >= 0 && nowSec >= sunriseSec && nowSec < sunriseSec + 120 && lastApplied != Theme::Light)
            {
                std::wcout << L"[Service] Sunrise event - switching to Light theme." << std::endl;
                app->applyTheme(Theme::Light, true);
                lastApplied = Theme::Light;
            }
            // Check if we crossed sunset (within last minute)
            else if (sunsetSec >= 0 && nowSec >= sunsetSec && nowSec < sunsetSec + 120 && lastApplied != Theme::Dark)
            {
                std::wcout << L"[Service] Sunset event - switching to Dark theme." << std::endl;
                app->applyTheme(Theme::Dark, true);
                lastApplied = Theme::Dark;
            }
            // Sync: if sun says Light but we applied Dark (or vice versa), correct
            else if (sunriseSec >= 0 && sunsetSec >= 0)
            {
                const bool shouldBeLight = (nowSec >= sunriseSec && nowSec < sunsetSec);
                if (shouldBeLight && lastApplied != Theme::Light)
                {
                    app->applyTheme(Theme::Light, true);
                    lastApplied = Theme::Light;
                }
                else if (!shouldBeLight && lastApplied != Theme::Dark)
                {
                    app->applyTheme(Theme::Dark, true);
                    lastApplied = Theme::Dark;
                }
            }
        }

        // Check WE health every 5 minutes
        static int healthCheckTicks = 0;
        if (++healthCheckTicks >= 5 && !app->weExePath_.empty())
        {
            healthCheckTicks = 0;
            if (!fileExists(app->weExePath_))
            {
                std::wcout << L"[Service] Wallpaper Engine path gone, re-detecting..." << std::endl;
                app->weExePath_ = app->detectWallpaperEnginePath();
            }
        }

        Sleep(60000); // 1 minute
    }

    // Service stopping
    app->serviceStatus_.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(app->serviceStatusHandle_, &app->serviceStatus_);
    return 0;
}

VOID WINAPI ServiceMain(DWORD argc, LPWSTR* argv)
{
    App* app = g_pServiceApp;
    if (!app) return;

    // Store status handle in a member temporarily
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = SERVICE_START_PENDING;
    status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    status.dwWaitHint = 3000;

    app->serviceStatusHandle_ = RegisterServiceCtrlHandlerW(L"Solune", ServiceCtrlHandler);
    if (!app->serviceStatusHandle_) return;

    // Use the app's SERVICE_STATUS for lifetime
    app->serviceStatus_ = status;
    SetServiceStatus(app->serviceStatusHandle_, &app->serviceStatus_);

    // Start worker thread
    HANDLE workerThread = CreateThread(nullptr, 0, App::serviceWorkerThread, app, 0, nullptr);
    if (!workerThread)
    {
        app->serviceStatus_.dwCurrentState = SERVICE_STOPPED;
        app->serviceStatus_.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
        SetServiceStatus(app->serviceStatusHandle_, &app->serviceStatus_);
        return;
    }

    // Wait for worker to complete
    WaitForSingleObject(workerThread, INFINITE);
    CloseHandle(workerThread);

    app->serviceStatus_.dwCurrentState = SERVICE_STOPPED;
    SetServiceStatus(app->serviceStatusHandle_, &app->serviceStatus_);
}

// ============================================================================
//  App::run — dispatch based on command line
// ============================================================================
int App::run()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    if (argc >= 2 && wcscmp(argv[1], L"--service") == 0)
    {
        // Windows Service mode
        g_pServiceApp = this;
        SERVICE_TABLE_ENTRYW table[] = {
            { const_cast<LPWSTR>(L"Solune"), ServiceMain },
            { nullptr, nullptr }
        };
        if (!StartServiceCtrlDispatcherW(table))
            std::wcerr << L"[Service] Failed to start service dispatcher." << std::endl;
        LocalFree(argv);
        return 0;
    }

    if (argc >= 2 && wcscmp(argv[1], L"--install") == 0)
    {
        // Register as Windows Service
        const std::wstring exePath = getExeDir() + L"\\Solune.exe";
        const std::wstring cmd = L"\"" + exePath + L"\" --service";

        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
        if (!scm)
        {
            std::wcerr << L"[Install] Failed to open SCM. Run as Administrator." << std::endl;
            LocalFree(argv);
            return 1;
        }

        // Delete existing service if any
        SC_HANDLE oldSvc = OpenServiceW(scm, L"Solune", DELETE);
        if (oldSvc) { DeleteService(oldSvc); CloseServiceHandle(oldSvc); }

        SC_HANDLE svc = CreateServiceW(
            scm, L"Solune", L"Solune Sun Theme Switcher",
            SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
            SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
            cmd.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);

        if (!svc)
        {
            std::wcerr << L"[Install] Failed to create service. Error: " << GetLastError() << std::endl;
            CloseServiceHandle(scm);
            LocalFree(argv);
            return 1;
        }

        // Configure service
        SERVICE_DESCRIPTIONW desc{};
        const wchar_t* descText = L"Automatically switches Windows Light/Dark theme based on sunrise and sunset.";
        desc.lpDescription = const_cast<LPWSTR>(descText);
        ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

        // Start the service
        StartServiceW(svc, 0, nullptr);

        std::wcout << L"[Install] Solune service installed and started." << std::endl;

        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        LocalFree(argv);
        return 0;
    }

    LocalFree(argv);

    // Default: run as console app (user session mode)
    try { winrt::init_apartment(); } catch (...) { return -1; }

    if (!acquireSingleInstance())
    {
        std::wcerr << L"[Solune] Already running." << std::endl;
        return 0;
    }

    config_ = loadConfig();
    weExePath_ = detectWallpaperEnginePath();

    if (!weExePath_.empty())
        std::wcout << L"[Solune] Wallpaper Engine: " << weExePath_ << std::endl;

    double lat = 0.0, lng = 0.0;
    if (!loadLocationFromConfig(lat, lng))
    {
        std::wcout << L"[Solune] Getting location..." << std::endl;
        localSun::Location loc = localSun::getLocationViaHttp();
        if (loc.valid)
        {
            lat = loc.latitude;
            lng = loc.longitude;
            saveLocationToConfig(lat, lng);
        }
        else
        {
            std::wcout << L"[Solune] IP geolocation failed. Using system time (7:00-19:00)." << std::endl;
        }
    }

    Theme expected = localSun::expectedThemeNow(lat, lng);
    applyTheme(expected, true);

    std::wcout << L"[Solune] Running (console mode). Close this window to stop." << std::endl;
    loop();

    releaseSingleInstance();
    return 0;
}

// ============================================================================
//  App::loop — console mode poll loop
// ============================================================================
void App::loop()
{
    double lat = 0.0, lng = 0.0;
    if (!loadLocationFromConfig(lat, lng))
    {
        // Try to get location once
        localSun::Location loc = localSun::getLocationViaHttp();
        if (loc.valid)
        {
            lat = loc.latitude;
            lng = loc.longitude;
            saveLocationToConfig(lat, lng);
        }
    }

    int lastDayOfYear = -1;
    long long sunriseSec = -1, sunsetSec = -1;
    Theme lastApplied = getSystemTheme();

    while (true)
    {
        SYSTEMTIME lt{};
        GetLocalTime(&lt);
        const int dayOfYear = lt.wYear * 366 + lt.wMonth * 31 + lt.wDay;

        if (dayOfYear != lastDayOfYear)
        {
            localSun::SunWindow window;
            if (localSun::getSunWindowForToday(window, lat, lng))
            {
                sunriseSec = window.sunriseSec;
                sunsetSec = window.sunsetSec;
                lastDayOfYear = dayOfYear;
            }
        }

        const long long nowSec = lt.wHour * 3600LL + lt.wMinute * 60LL + lt.wSecond;

        const bool shouldBeLight = (sunriseSec >= 0 && sunsetSec >= 0 &&
                                    nowSec >= sunriseSec && nowSec < sunsetSec);

        if (shouldBeLight && lastApplied != Theme::Light)
        {
            std::wcout << L"[Solune] Sunrise - switching to Light." << std::endl;
            applyTheme(Theme::Light, true);
            lastApplied = Theme::Light;
        }
        else if (!shouldBeLight && lastApplied != Theme::Dark)
        {
            std::wcout << L"[Solune] Sunset - switching to Dark." << std::endl;
            applyTheme(Theme::Dark, true);
            lastApplied = Theme::Dark;
        }

        // Check WE
        if (!weExePath_.empty() && !fileExists(weExePath_))
        {
            std::wcout << L"[Solune] WE path gone, re-detecting..." << std::endl;
            weExePath_ = detectWallpaperEnginePath();
        }

        Sleep(60000); // 1 minute
    }
}

} // namespace sts
