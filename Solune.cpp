#include "Solune.h"
#include "WeConfigManager.h"
#include "StringConvert.h"

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

#include <winrt/Windows.Devices.Geolocation.h>
#include <winrt/Windows.Foundation.h>

#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "windowsapp.lib")

namespace fs = std::filesystem;

namespace sts
{
static bool fileExists(const std::wstring& path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

namespace registry
{
static bool readDword(HKEY root, const wchar_t* subKey, const wchar_t* valueName, DWORD& out)
{
    HKEY hKey{};
    if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return false;

    DWORD type = 0;
    DWORD cb = sizeof(DWORD);
    DWORD value = 0;
    const LONG rc = RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<BYTE*>(&value), &cb);
    RegCloseKey(hKey);
    if (rc == ERROR_SUCCESS && type == REG_DWORD)
    {
        out = value;
        return true;
    }
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

static FILETIME getFileWriteTime(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr))
        return attr.ftLastWriteTime;
    return {0, 0};
}

static int compareFileTime(FILETIME f1, FILETIME f2)
{
    return CompareFileTime(&f1, &f2);
}

namespace localSun
{
struct Location
{
    double latitude = 0.0;
    double longitude = 0.0;
    bool valid = false;
};

struct SunWindow
{
    int sunriseSec = -1;
    int sunsetSec = -1;
    bool valid = false;
};

static constexpr double kPi = 3.141592653589793;
static constexpr double kRadPerDeg = kPi / 180.0;
static constexpr double kDegPerRad = 180.0 / kPi;
static constexpr int kSecPerMin = 60;
static constexpr int kSecPerHour = 3600;
static constexpr int kSecPerDay = 86400;

static double degToRad(double x)
{
    return x * kRadPerDeg;
}

static double radToDeg(double x)
{
    return x * kDegPerRad;
}

static Location getLocation()
{
    Location loc;
    try
    {
        winrt::Windows::Devices::Geolocation::Geolocator geolocator;
        geolocator.DesiredAccuracy(winrt::Windows::Devices::Geolocation::PositionAccuracy::Default);
        const auto pos = geolocator.GetGeopositionAsync().get();
        const auto coord = pos.Coordinate().Point().Position();
        loc.latitude = coord.Latitude;
        loc.longitude = coord.Longitude;
        loc.valid = true;
    }
    catch (...)
    {
        loc.valid = false;
    }
    return loc;
}

static int roundHoursToSeconds(double hours)
{
    long long seconds = static_cast<long long>(hours * static_cast<double>(kSecPerHour) + 0.5);
    seconds %= kSecPerDay;
    if (seconds < 0)
        seconds += kSecPerDay;
    return static_cast<int>(seconds);
}

static bool getSunWindowForToday(SunWindow& out)
{
    static WORD cachedYear = 0;
    static WORD cachedMonth = 0;
    static WORD cachedDay = 0;
    static SunWindow cachedWindow{};

    SYSTEMTIME lt{};
    GetLocalTime(&lt);
    if (cachedWindow.valid && cachedYear == lt.wYear && cachedMonth == lt.wMonth && cachedDay == lt.wDay)
    {
        out = cachedWindow;
        return true;
    }

    const Location loc = getLocation();
    if (!loc.valid)
    {
        cachedWindow = {};
        out = cachedWindow;
        return false;
    }

    const int year = static_cast<int>(lt.wYear);
    const int month = static_cast<int>(lt.wMonth);
    const int day = static_cast<int>(lt.wDay);
    const int n1 = static_cast<int>((275 * month) / 9);
    const int n2 = static_cast<int>((month + 9) / 12);
    const int n3 = 1 + static_cast<int>((year - ((year >> 2) << 2) + 2) / 3);
    const int N = n1 - (n2 * n3) + day - 30;
    const double lngHour = loc.longitude / 15.0;

    auto calcLocalHours = [&](bool sunrise) -> double
    {
        const double t = N + ((sunrise ? 6.0 : 18.0) - lngHour) / 24.0;
        const double M = (0.9856 * t) - 3.289;
        double L = M + (1.916 * std::sin(degToRad(M))) + (0.020 * std::sin(degToRad(M + M))) + 282.634;
        while (L < 0.0)
            L += 360.0;
        while (L >= 360.0)
            L -= 360.0;

        double RA = radToDeg(std::atan(0.91764 * std::tan(degToRad(L))));
        while (RA < 0.0)
            RA += 360.0;
        while (RA >= 360.0)
            RA -= 360.0;

        const double Lquadrant = std::floor(L / 90.0) * 90.0;
        const double RAquadrant = std::floor(RA / 90.0) * 90.0;
        RA = (RA + (Lquadrant - RAquadrant)) / 15.0;

        const double sinDec = 0.39782 * std::sin(degToRad(L));
        const double cosDec = std::cos(std::asin(sinDec));
        const double cosH = (std::cos(degToRad(90.833)) - (sinDec * std::sin(degToRad(loc.latitude)))) /
                            (cosDec * std::cos(degToRad(loc.latitude)));
        if (cosH > 1.0 || cosH < -1.0)
            return -1.0;

        double H = sunrise ? (360.0 - radToDeg(std::acos(cosH))) : radToDeg(std::acos(cosH));
        H /= 15.0;

        const double T = H + RA - (0.06571 * t) - 6.622;
        double UT = T - lngHour;
        while (UT < 0.0)
            UT += 24.0;
        while (UT >= 24.0)
            UT -= 24.0;

        TIME_ZONE_INFORMATION tzi{};
        GetTimeZoneInformation(&tzi);
        double local = UT - (tzi.Bias / 60.0);
        while (local < 0.0)
            local += 24.0;
        while (local >= 24.0)
            local -= 24.0;
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
    cachedYear = lt.wYear;
    cachedMonth = lt.wMonth;
    cachedDay = lt.wDay;
    out = cachedWindow;
    return true;
}

enum class SunEvent
{
    None,
    Sunrise,
    Sunset
};

static SunEvent checkSunEventNow(int toleranceSeconds, bool firstRequest)
{
    (void)firstRequest;

    SunWindow window;
    if (!getSunWindowForToday(window) || !window.valid)
        return SunEvent::None;

    SYSTEMTIME lt{};
    GetLocalTime(&lt);
    const int nowSec = (lt.wHour * kSecPerHour) + (lt.wMinute * kSecPerMin) + lt.wSecond;

    if (std::abs(nowSec - window.sunriseSec) <= toleranceSeconds)
        return SunEvent::Sunrise;
    if (std::abs(nowSec - window.sunsetSec) <= toleranceSeconds)
        return SunEvent::Sunset;
    return SunEvent::None;
}

static Theme expectedThemeNow()
{
    SunWindow window;
    if (!getSunWindowForToday(window) || !window.valid)
        return Theme::Dark;

    SYSTEMTIME lt{};
    GetLocalTime(&lt);
    const int nowSec = (lt.wHour * kSecPerHour) + (lt.wMinute * kSecPerMin) + lt.wSecond;
    return (nowSec >= window.sunriseSec && nowSec < window.sunsetSec) ? Theme::Light : Theme::Dark;
}
} // namespace localSun

bool App::acquireSingleInstance()
{
    mutex_ = CreateMutexA(nullptr, TRUE, "Global\\SoluneMutex");
    if (!mutex_ || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (mutex_)
            CloseHandle(mutex_);
        mutex_ = nullptr;
        return false;
    }
    return true;
}

void App::releaseSingleInstance()
{
    if (mutex_)
    {
        ReleaseMutex(mutex_);
        CloseHandle(mutex_);
        mutex_ = nullptr;
    }
}

void App::ensureAutoRun()
{
    HKEY hKey{};
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE,
                      &hKey) != ERROR_SUCCESS)
        return;

    char path[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    const std::string command = std::string("\"") + path + "\"";
    RegSetValueExA(hKey, "Solune", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                   static_cast<DWORD>(command.size() + 1));
    RegCloseKey(hKey);
}

static bool readRegistryStringW(HKEY root, const wchar_t* subKey, const wchar_t* valueName, std::wstring& out,
                                REGSAM wowFlags = 0)
{
    HKEY hKey{};
    if (RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE | wowFlags, &hKey) != ERROR_SUCCESS)
        return false;

    DWORD type = 0;
    DWORD bytes = 0;
    LONG rc = RegQueryValueExW(hKey, valueName, nullptr, &type, nullptr, &bytes);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0)
    {
        RegCloseKey(hKey);
        return false;
    }

    std::wstring buffer((bytes + sizeof(wchar_t) - 1) / sizeof(wchar_t), L'\0');
    rc = RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buffer.data()), &bytes);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS)
        return false;

    while (!buffer.empty() && buffer.back() == L'\0')
        buffer.pop_back();

    if (type == REG_EXPAND_SZ)
    {
        const DWORD needed = ExpandEnvironmentStringsW(buffer.c_str(), nullptr, 0);
        if (needed > 0)
        {
            std::wstring expanded(needed, L'\0');
            const DWORD written = ExpandEnvironmentStringsW(buffer.c_str(), expanded.data(), needed);
            if (written > 0 && written <= needed)
            {
                while (!expanded.empty() && expanded.back() == L'\0')
                    expanded.pop_back();
                buffer = std::move(expanded);
            }
        }
    }

    out = buffer;
    return !out.empty();
}

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

static std::wstring unescapeVdfPath(std::string value)
{
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i)
    {
        if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '\\')
        {
            out.push_back('\\');
            ++i;
        }
        else
        {
            out.push_back(value[i]);
        }
    }
    return normalizeWindowsPath(sts::WStringFromUtf8(out));
}

static void appendUniquePath(std::vector<std::wstring>& paths, std::unordered_set<std::wstring>& seen, std::wstring path)
{
    path = normalizeWindowsPath(std::move(path));
    if (path.empty())
        return;
    if (seen.insert(path).second)
        paths.push_back(std::move(path));
}

static std::wstring getEnvString(const wchar_t* name)
{
    wchar_t* buffer = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&buffer, &len, name) != 0 || !buffer || len == 0)
    {
        free(buffer);
        return {};
    }
    const std::wstring value(buffer);
    free(buffer);
    return value;
}

static std::wstring findProcessImagePath(const wchar_t* exeName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return {};

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(snap, &pe))
    {
        CloseHandle(snap);
        return {};
    }

    do
    {
        if (_wcsicmp(pe.szExeFile, exeName) != 0)
            continue;

        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!process)
            continue;

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

static bool isWallpaperEngineProcessRunning()
{
    return !findProcessImagePath(L"wallpaper64.exe").empty() || !findProcessImagePath(L"wallpaper32.exe").empty();
}

static void appendSteamInstallCandidates(std::vector<std::wstring>& roots)
{
    std::unordered_set<std::wstring> seen;
    for (const auto& p : roots)
    {
        if (!p.empty())
            seen.insert(normalizeWindowsPath(p));
    }

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

    if (!pf86.empty())
        appendUniquePath(roots, seen, pf86 + L"\\Steam");
    if (!pf.empty())
        appendUniquePath(roots, seen, pf + L"\\Steam");
    if (!localAppData.empty())
        appendUniquePath(roots, seen, localAppData + L"\\Programs\\Steam");
}

static std::vector<std::wstring> parseSteamLibraries(const std::wstring& steamPath)
{
    std::vector<std::wstring> libraries;
    std::unordered_set<std::wstring> seen;
    appendUniquePath(libraries, seen, steamPath);

    const std::wstring vdfPath = normalizeWindowsPath(steamPath) + L"\\steamapps\\libraryfolders.vdf";
    std::ifstream file(fs::path(vdfPath), std::ios::binary);
    if (!file.is_open())
        return libraries;

    std::string line;
    while (std::getline(file, line))
    {
        const auto pathTag = line.find("\"path\"");
        if (pathTag == std::string::npos)
            continue;

        const auto firstQuote = line.find('"', pathTag + 6);
        if (firstQuote == std::string::npos)
            continue;
        const auto secondQuote = line.find('"', firstQuote + 1);
        if (secondQuote == std::string::npos || secondQuote <= firstQuote + 1)
            continue;

        appendUniquePath(libraries, seen, unescapeVdfPath(line.substr(firstQuote + 1, secondQuote - firstQuote - 1)));
    }
    return libraries;
}

std::wstring App::detectWallpaperEnginePath()
{
    const std::wstring runningPath = findProcessImagePath(L"wallpaper64.exe");
    if (!runningPath.empty() && fileExists(runningPath))
        return runningPath;

    std::vector<std::wstring> steamRoots;
    appendSteamInstallCandidates(steamRoots);

    std::unordered_set<std::wstring> seenLibraries;
    std::vector<std::wstring> libraries;
    for (const auto& root : steamRoots)
    {
        for (const auto& lib : parseSteamLibraries(root))
            appendUniquePath(libraries, seenLibraries, lib);
    }

    static const wchar_t* kExeNames[] = {L"wallpaper64.exe", L"wallpaper32.exe"};
    for (const auto& lib : libraries)
    {
        for (const wchar_t* exeName : kExeNames)
        {
            const std::wstring commonPath = lib + L"\\steamapps\\common\\wallpaper_engine\\" + exeName;
            if (fileExists(commonPath))
                return commonPath;

            const std::wstring directPath = lib + L"\\" + exeName;
            if (fileExists(directPath))
                return directPath;
        }
    }
    return {};
}

Theme App::getSystemTheme()
{
    const wchar_t* personalizeKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    DWORD systemLight = 1;
    if (registry::readDword(HKEY_CURRENT_USER, personalizeKey, L"SystemUsesLightTheme", systemLight) &&
        systemLight == 1)
        return Theme::Light;
    return Theme::Dark;
}

static void broadcastThemeRefresh()
{
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet"),
                        SMTO_ABORTIFHUNG, 2000, &result);
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"), SMTO_ABORTIFHUNG,
                        2000, &result);
    SendMessageTimeoutW(HWND_BROADCAST, WM_THEMECHANGED, 0, 0, SMTO_ABORTIFHUNG, 2000, &result);

    auto refreshWindow = [](HWND hwnd)
    {
        if (!hwnd)
            return;
        InvalidateRect(hwnd, nullptr, TRUE);
        UpdateWindow(hwnd);
        PostMessageW(hwnd, WM_THEMECHANGED, 0, 0);
    };

    refreshWindow(FindWindowW(L"Shell_TrayWnd", nullptr));
    HWND sec = FindWindowW(L"Shell_SecondaryTrayWnd", nullptr);
    while (sec)
    {
        refreshWindow(sec);
        sec = FindWindowExW(nullptr, sec, L"Shell_SecondaryTrayWnd", nullptr);
    }
}

static void repairAccentColorAfterWallpaperSwitch()
{
    static constexpr const wchar_t* kPersonalizeKey =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    static constexpr const wchar_t* kDwmKey = L"Software\\Microsoft\\Windows\\DWM";
    static constexpr const wchar_t* kDesktopKey = L"Control Panel\\Desktop";

    DWORD taskbarAccent = 0;
    DWORD titleBarAccent = 0;
    DWORD autoColorization = 0;

    // 读取当前用户原本的开关状态
    registry::readDword(HKEY_CURRENT_USER, kPersonalizeKey, L"ColorPrevalence", taskbarAccent);
    registry::readDword(HKEY_CURRENT_USER, kDwmKey, L"ColorPrevalence", titleBarAccent);
    registry::readDword(HKEY_CURRENT_USER, kDesktopKey, L"AutoColorization", autoColorization);

    // 先做一次普通刷新
    broadcastThemeRefresh();
    Sleep(120);

    // 如果用户启用了“自动从背景选取强调色”，强制抖一下这个开关，逼系统重新按当前壁纸取色
    if (autoColorization == 1)
    {
        registry::writeDword(HKEY_CURRENT_USER, kDesktopKey, L"AutoColorization", 0);
        broadcastThemeRefresh();
        Sleep(120);

        registry::writeDword(HKEY_CURRENT_USER, kDesktopKey, L"AutoColorization", 1);
        Sleep(120);
    }

    // 把当前用户原本的强调色显示开关重新压回去，确保标题栏/边框这一项被 DWM 重新吃到
    registry::writeDword(HKEY_CURRENT_USER, kPersonalizeKey, L"ColorPrevalence", taskbarAccent);
    registry::writeDword(HKEY_CURRENT_USER, kDwmKey, L"ColorPrevalence", titleBarAccent);

    broadcastThemeRefresh();
}

static bool launchWallpaperEngine(const std::wstring& wallpaperExePath, const std::wstring& playlistName)
{
    if (wallpaperExePath.empty() || playlistName.empty())
        return false;

    std::wstring cmd = L"\"" + wallpaperExePath + L"\" -control openPlaylist -playlist \"" + playlistName + L"\"";
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    const BOOL ok =
        CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok)
        return false;

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void App::applyTheme(Theme targetTheme, bool switchWallpaper)
{
    static constexpr const wchar_t* kPersonalizeKey =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    static constexpr const wchar_t* kDwmKey = L"Software\\Microsoft\\Windows\\DWM";

    const DWORD themeValue = (targetTheme == Theme::Dark) ? 0u : 1u;
    const DWORD accentVisible = (targetTheme == Theme::Dark) ? 1u : 0u;

    registry::writeDword(HKEY_CURRENT_USER, kPersonalizeKey, L"SystemUsesLightTheme", themeValue);
    registry::writeDword(HKEY_CURRENT_USER, kPersonalizeKey, L"AppsUseLightTheme", themeValue);

    // 这项保留给开始菜单/任务栏
    registry::writeDword(HKEY_CURRENT_USER, kPersonalizeKey, L"ColorPrevalence", accentVisible);

    // 这项才是“在标题栏和窗口边框上显示强调色”
    registry::writeDword(HKEY_CURRENT_USER, kDwmKey, L"ColorPrevalence", accentVisible);

    broadcastThemeRefresh();

    bool launchedWe = false;
    if (switchWallpaper && !weExePath_.empty())
    {
        const std::wstring playlistName = (targetTheme == Theme::Dark) ? L"black_auto" : L"white_auto";
        launchedWe = launchWallpaperEngine(weExePath_, playlistName);
    }

    // 如果刚切了壁纸，给壁纸引擎一点落盘/渲染时间，再修复“壁纸取色 -> 标题栏强调色”链路
    if (launchedWe)
        Sleep(1500);

    repairAccentColorAfterWallpaperSwitch();
}

void App::loop()
{
    auto lastEvent = localSun::checkSunEventNow(60, true);
    int ticks = 0;
    int rediscoverTicks = 0;

    bool hasWallpaperEngine = false;
    std::wstring workshopRoot;
    std::wstring myProjectsRoot;

    auto connectWallpaperEngine = [&]() -> bool
    {
        std::wstring detectedPath = detectWallpaperEnginePath();
        if (detectedPath.empty())
            return false;

        const fs::path weExe(detectedPath);
        const fs::path weDir = weExe.parent_path();
        const std::wstring configPath = (weDir / "config.json").wstring();

        if (!fs::exists(fs::path(configPath)))
            return false;

        weExePath_ = std::move(detectedPath);
        configPathW_ = configPath;
        workshopRoot = (weDir.parent_path().parent_path() / "workshop" / "content" / "431960").wstring();
        myProjectsRoot = (weDir / "projects" / "myprojects").wstring();
        lastConfigWriteTime_ = getFileWriteTime(configPathW_);
        hasWallpaperEngine = true;
        return true;
    };

    auto disconnectWallpaperEngine = [&]()
    {
        weExePath_.clear();
        configPathW_.clear();
        workshopRoot.clear();
        myProjectsRoot.clear();
        lastConfigWriteTime_ = {0, 0};
        hasWallpaperEngine = false;
    };

    if (connectWallpaperEngine())
    {
        std::wcout << L"[联动] Wallpaper Engine 联动已启用。" << std::endl;
    }
    else
    {
        std::wcout << L"[联动] 当前未检测到 Wallpaper Engine，运行于仅系统主题模式。" << std::endl;
    }

    while (true)
    {
        ++ticks;
        ++rediscoverTicks;

        if (ticks >= 10)
        {
            ticks = 0;
            const auto ev = localSun::checkSunEventNow(60, false);
            if (ev != lastEvent)
            {
                lastEvent = ev;
                if (ev == localSun::SunEvent::Sunrise)
                    applyTheme(Theme::Light);
                else if (ev == localSun::SunEvent::Sunset)
                    applyTheme(Theme::Dark);
            }
        }

        if (hasWallpaperEngine)
        {
            if (!fs::exists(fs::path(configPathW_)) || !fileExists(weExePath_))
            {
                std::wcout << L"[联动] 检测到 Wallpaper Engine 已退出或配置文件不可用，已自动降级为仅系统主题模式。"
                           << std::endl;
                disconnectWallpaperEngine();
                rediscoverTicks = 0;
                Sleep(1000);
                continue;
            }

            const FILETIME currentFt = getFileWriteTime(configPathW_);
            if (compareFileTime(currentFt, lastConfigWriteTime_) > 0)
            {
                lastConfigWriteTime_ = currentFt;
                Sleep(4000);

                const Theme expectedTheme = localSun::expectedThemeNow();
                const sts::we::ThemeTag targetTag =
                    (expectedTheme == Theme::Light) ? sts::we::ThemeTag::Light : sts::we::ThemeTag::Dark;

                sts::we::ApplyOptions opt;
                opt.lightAutoPlaylistName = L"white_auto";
                opt.darkAutoPlaylistName = L"black_auto";
                opt.configPath = configPathW_;
                opt.workshopRoot431960 = workshopRoot;
                opt.myProjectsRoot = myProjectsRoot;
                opt.desiredTheme = targetTag;
                opt.forceReclassifyExistingTags = false;
                opt.printDiagnostics = true;
                opt.manageWallpaperEngineProcess = false;

                std::wcout << L"\n[同步] 检测到配置变更，开始静默同步用户壁纸..." << std::endl;
                const sts::we::UpdateResult res = sts::we::ApplyAndSwitch(opt);

                if (res.changed)
                {
                    std::wcout << L"[同步] 已完成配置回写与播放列表同步。" << std::endl;
                }

                // 用户可能在 WE 中手动切换了壁纸，无论播放列表是否变化，都强制 Windows 重新从当前壁纸取色
                Sleep(1200);
                repairAccentColorAfterWallpaperSwitch();

                lastConfigWriteTime_ = getFileWriteTime(configPathW_);
            }
        }
        else
        {
            if (rediscoverTicks >= 5)
            {
                rediscoverTicks = 0;

                if (connectWallpaperEngine())
                {
                    std::wcout << L"[联动] 检测到 Wallpaper Engine 已恢复，正在重新接管壁纸联动..." << std::endl;

                    const Theme expectedTheme = localSun::expectedThemeNow();
                    const sts::we::ThemeTag targetTag =
                        (expectedTheme == Theme::Light) ? sts::we::ThemeTag::Light : sts::we::ThemeTag::Dark;

                    sts::we::ApplyOptions opt;
                    opt.lightAutoPlaylistName = L"white_auto";
                    opt.darkAutoPlaylistName = L"black_auto";
                    opt.configPath = configPathW_;
                    opt.workshopRoot431960 = workshopRoot;
                    opt.myProjectsRoot = myProjectsRoot;
                    opt.desiredTheme = targetTag;
                    opt.forceReclassifyExistingTags = false;
                    opt.printDiagnostics = true;
                    opt.manageWallpaperEngineProcess = false;

                    const sts::we::UpdateResult res = sts::we::ApplyAndSwitch(opt);
                    if (!res.error.empty())
                        std::wcerr << L"[警告] 恢复联动时 ApplyAndSwitch 返回信息：" << res.error << std::endl;

                    const bool switchWallpaper =
                        !(res.activePlaylistAlreadySuitable && isWallpaperEngineProcessRunning());
                    applyTheme(expectedTheme, switchWallpaper);
                    lastConfigWriteTime_ = getFileWriteTime(configPathW_);

                    std::wcout << L"[联动] Wallpaper Engine 联动已恢复。" << std::endl;
                }
            }
        }

        Sleep(1000);
    }
}

int App::run()
{
    ensureAutoRun();
    if (!acquireSingleInstance())
    {
        std::wcerr << L"[信息] Solune 已在后台运行，本次启动将直接退出。" << std::endl;
        return 0;
    }

    weExePath_ = detectWallpaperEnginePath();
    const Theme expectedTheme = localSun::expectedThemeNow();
    bool switchWallpaperOnThemeApply = false;

    if (!weExePath_.empty())
    {
        const fs::path weExe(weExePath_);
        const fs::path weDir = weExe.parent_path();
        configPathW_ = (weDir / "config.json").wstring();

        if (fs::exists(fs::path(configPathW_)))
        {
            const sts::we::ThemeTag targetTag =
                (expectedTheme == Theme::Light) ? sts::we::ThemeTag::Light : sts::we::ThemeTag::Dark;

            sts::we::ApplyOptions opt;
            opt.lightAutoPlaylistName = L"white_auto";
            opt.darkAutoPlaylistName = L"black_auto";
            opt.configPath = configPathW_;
            opt.workshopRoot431960 = (weDir.parent_path().parent_path() / "workshop" / "content" / "431960").wstring();
            opt.myProjectsRoot = (weDir / "projects" / "myprojects").wstring();
            opt.desiredTheme = targetTag;
            opt.forceReclassifyExistingTags = false;
            opt.printDiagnostics = true;

            std::wcout << L"[启动] 已检测到 Wallpaper Engine，开始执行静态校准与播放列表同步..." << std::endl;
            const sts::we::UpdateResult res = sts::we::ApplyAndSwitch(opt);
            if (!res.error.empty())
                std::wcerr << L"[警告] ApplyAndSwitch 返回信息：" << res.error << std::endl;

            switchWallpaperOnThemeApply = !(res.activePlaylistAlreadySuitable && isWallpaperEngineProcessRunning());
            if (!switchWallpaperOnThemeApply && res.activePlaylistPreserved)
                std::wcout << L"[启动] 当前播放列表符合主题要求，保留 Wallpaper Engine 已选壁纸。" << std::endl;

            lastConfigWriteTime_ = getFileWriteTime(configPathW_);
        }
        else
        {
            std::wcout << L"[启动] 已检测到 Wallpaper Engine，但未找到 config.json，已降级为仅系统主题自动切换模式。"
                       << std::endl;
            weExePath_.clear();
            configPathW_.clear();
            lastConfigWriteTime_ = {0, 0};
        }
    }
    else
    {
        std::wcout << L"[启动] 未检测到 Wallpaper Engine，已启用仅系统深浅色主题自动切换模式。" << std::endl;
        configPathW_.clear();
        lastConfigWriteTime_ = {0, 0};
    }

    applyTheme(expectedTheme, switchWallpaperOnThemeApply);

    std::wcout << L"----------------------------------------" << std::endl;
    std::wcout << L"[运行] Solune 已进入后台静默守护状态，你现在可以关闭此窗口。" << std::endl;

    loop();

    releaseSingleInstance();
    return 0;
}
} // namespace sts
