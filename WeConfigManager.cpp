#include "WeConfigManager.h"
#include "StringConvert.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <shellapi.h>
#include <tlhelp32.h>
#include <wincodec.h>
#include <windows.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include <cstdint>
#include <cstring>
#include <string_view>
#include <thread>


#include <PropIdl.h>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "Windowscodecs.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")


namespace sts::we
{
using winrt::Windows::Data::Json::JsonArray;
using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::JsonValueType;

// ============================================================
// 提前声明区 (彻底解决“找不到标识符”报错)
// ============================================================
static bool isProcessRunningByName(const wchar_t* exeName);
static bool isWallpaperEngineRunning();
static void killWallpaperEngineHard();
static bool waitProcessExitByName(unsigned timeoutMs);
static bool tryGracefulExitWallpaperEngine(unsigned gracefulTimeoutMs);
static bool waitFileWritable(const std::wstring& path, unsigned timeoutMs);
static std::wstring inferWallpaperEngineExe(const std::wstring& installDir);
static bool startWallpaperEngineExe(const std::wstring& exePath, std::wstring& err);
static void sleepMs(unsigned ms);
static std::vector<DWORD> collectPidsByName(const wchar_t* exeName);
static void requestCloseProcessWindows(DWORD pid);

static int findPlaylistIndexByName(JsonArray const& playlists, const std::wstring& name);
static bool arrayContainsString(JsonArray const& arr, const std::wstring& s);
static bool removeStringFromArray(JsonArray& arr, const std::wstring& s);
static bool removeWallpaperFromArray(JsonArray& arr, const std::wstring& s, bool keepExactMatch);
static bool addUniqueString(JsonArray& arr, const std::wstring& s);
static bool ensurePlaylist(JsonArray& playlists, const std::wstring& name, JsonObject const& settingsTemplate,
                           int& outIndex);
static void ensureMonitor0Object(JsonObject& entryObj, JsonObject& outMonitor0);
static bool tryReadThemeTag(JsonObject const& monitor0, ThemeTag& out);
static bool writeThemeTag(JsonObject& monitor0, ThemeTag tag);
static bool ensureAndGetPlaylists(JsonObject& general, JsonArray& outPlaylists, std::wstring& err);
static bool isActivePlaylistSuitableForTheme(JsonObject const& general, JsonArray const& playlists,
                                             ThemeTag desiredTheme, const std::wstring& lightAuto,
                                             const std::wstring& darkAuto, JsonArray const& lightItems,
                                             JsonArray const& darkItems);
static bool setActivePlaylist(JsonObject& general, JsonArray const& playlists, const std::wstring& playlistName,
                              bool& changed);

static bool tryGetProjectJsonSchemecolor(const std::wstring& pj, std::wstring& outScheme);
static bool tryGetMainWallpaperKeyFromProjectJson(const std::wstring& pj, std::wstring& outKey);
static bool tryGetPreviewPathFromProjectJson(const std::wstring& projectJsonPath, std::wstring& outPreviewPath);
static bool jsonTryGetNumber(JsonObject const& obj, const std::wstring& key, double& out);
static bool calcImageRoiStatsWIC(const std::wstring& imagePath, double wPct, double hPct, double& outAvgLuminance,
                                 double& outDarkRatio);

// ============================================================
// 基础工具函数
// ============================================================
static std::wstring toLower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c)
                   {
                       if (c >= L'A' && c <= L'Z')
                           return wchar_t(c - L'A' + L'a');
                       return c;
                   });
    return s;
}
static std::wstring normalizeSlashes(std::wstring s)
{
    for (auto& c : s)
        if (c == L'\\')
            c = L'/';
    return s;
}
static std::wstring canonicalizePathKey(std::wstring p)
{
    p = normalizeSlashes(std::move(p));
    if (p.size() >= 2 && p[1] == L':' && p[0] >= L'a' && p[0] <= L'z')
        p[0] = wchar_t(p[0] - L'a' + L'A');
    std::wstring out;
    out.reserve(p.size());
    wchar_t prev = 0;
    for (size_t i = 0; i < p.size(); ++i)
    {
        const wchar_t ch = p[i];
        const bool keepUncPrefix = (ch == L'/' && prev == L'/' && i == 1 && out.size() == 1 && out[0] == L'/');
        if (ch == L'/' && prev == L'/' && !keepUncPrefix)
            continue;
        out.push_back(ch);
        prev = ch;
    }
    while (out.size() > 1 && out.back() == L'/')
    {
        if (out.size() == 3 && out[1] == L':')
            break;
        out.pop_back();
    }
    return out;
}
static std::wstring canonicalPathCompareKey(const std::wstring& p)
{
    return toLower(canonicalizePathKey(p));
}
static bool samePathKey(const std::wstring& a, const std::wstring& b)
{
    return canonicalPathCompareKey(a) == canonicalPathCompareKey(b);
}
static bool findObjectPathKey(JsonObject const& obj, const std::wstring& key, std::wstring& outExactKey)
{
    if (obj.HasKey(key))
    {
        outExactKey = key;
        return true;
    }

    const std::wstring target = canonicalPathCompareKey(key);
    for (auto const& kv : obj)
    {
        if (canonicalPathCompareKey(kv.Key().c_str()) == target)
        {
            outExactKey = kv.Key().c_str();
            return true;
        }
    }
    return false;
}
static bool fileExists(const std::wstring& p)
{
    DWORD attr = GetFileAttributesW(p.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}
static bool dirExists(const std::wstring& p)
{
    DWORD attr = GetFileAttributesW(p.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
}
static bool endsWithInsensitive(const std::wstring& value, const std::wstring& suffix)
{
    if (value.size() < suffix.size())
        return false;
    return toLower(value.substr(value.size() - suffix.size())) == toLower(suffix);
}
static std::wstring joinPath(const std::wstring& a, const std::wstring& b)
{
    if (a.empty())
        return b;
    if (b.empty())
        return a;
    if (a.back() == L'\\' || a.back() == L'/')
        return a + b;
    return a + L'\\' + b;
}
static std::wstring getParentDir(std::wstring path)
{
    path = canonicalizePathKey(std::move(path));
    auto pos = path.find_last_of(L'/');
    if (pos == std::wstring::npos)
        return L"";
    return path.substr(0, pos);
}
static std::wstring getWallpaperDir(const std::wstring& path)
{
    std::wstring p = canonicalizePathKey(path);
    if (!p.empty() && p.back() == L'/')
    {
        p.pop_back();
        return p;
    }
    auto slashPos = p.find_last_of(L'/');
    auto dotPos = p.find_last_of(L'.');
    if (dotPos != std::wstring::npos && (slashPos == std::wstring::npos || dotPos > slashPos))
        return getParentDir(p);
    return p;
}
static bool tryResolveExistingWallpaperFile(const std::wstring& wallpaperPath, std::wstring& outFileKey)
{
    const std::wstring p = canonicalizePathKey(wallpaperPath);
    if (fileExists(p))
    {
        outFileKey = p;
        return true;
    }

    const std::wstring dir = dirExists(p) ? p : getWallpaperDir(p);
    if (dir.empty() || !dirExists(dir))
        return false;

    if (endsWithInsensitive(p, L"/scene.json"))
    {
        std::wstring pkgPath = canonicalizePathKey(joinPath(dir, L"scene.pkg"));
        if (fileExists(pkgPath))
        {
            outFileKey = pkgPath;
            return true;
        }
    }
    if (endsWithInsensitive(p, L"/scene.pkg"))
    {
        std::wstring sceneJsonPath = canonicalizePathKey(joinPath(dir, L"scene.json"));
        if (fileExists(sceneJsonPath))
        {
            outFileKey = sceneJsonPath;
            return true;
        }
    }

    std::wstring projectJsonPath = canonicalizePathKey(joinPath(dir, L"project.json"));
    std::wstring mainKey;
    if (fileExists(projectJsonPath) && tryGetMainWallpaperKeyFromProjectJson(projectJsonPath, mainKey))
    {
        mainKey = canonicalizePathKey(mainKey);
        if (fileExists(mainKey))
        {
            outFileKey = mainKey;
            return true;
        }

        const std::wstring mainDir = getWallpaperDir(mainKey);
        if (!mainDir.empty() && dirExists(mainDir))
        {
            if (endsWithInsensitive(mainKey, L"/scene.json"))
            {
                std::wstring pkgPath = canonicalizePathKey(joinPath(mainDir, L"scene.pkg"));
                if (fileExists(pkgPath))
                {
                    outFileKey = pkgPath;
                    return true;
                }
            }
            else if (endsWithInsensitive(mainKey, L"/scene.pkg"))
            {
                std::wstring sceneJsonPath = canonicalizePathKey(joinPath(mainDir, L"scene.json"));
                if (fileExists(sceneJsonPath))
                {
                    outFileKey = sceneJsonPath;
                    return true;
                }
            }
        }
    }

    std::wstring pkgPath = canonicalizePathKey(joinPath(dir, L"scene.pkg"));
    if (fileExists(pkgPath))
    {
        outFileKey = pkgPath;
        return true;
    }

    std::wstring sceneJsonPath = canonicalizePathKey(joinPath(dir, L"scene.json"));
    if (fileExists(sceneJsonPath))
    {
        outFileKey = sceneJsonPath;
        return true;
    }

    return false;
}
static bool wallpaperFileExists(const std::wstring& wallpaperPath)
{
    std::wstring resolved;
    return tryResolveExistingWallpaperFile(wallpaperPath, resolved);
}
static std::string utf16ToUtf8(const std::wstring& ws)
{
    if (ws.empty())
        return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), out.data(), len, nullptr, nullptr);
    return out;
}
static bool readAllTextUtf8(const std::wstring& path, std::string& out)
{
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
        return false;
    out.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return true;
}
static bool writeAllTextUtf8(const std::wstring& path, const std::string& text)
{
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs)
        return false;
    ofs.write(text.data(), static_cast<std::streamsize>(text.size()));
    return ofs.good();
}
static bool atomicReplaceFile(const std::wstring& targetPath, const std::string& newContent, std::wstring& err)
{
    std::wstring tmpPath = targetPath + L".tmp";
    if (!writeAllTextUtf8(tmpPath, newContent))
    {
        err = L"Failed to write tmp file.";
        return false;
    }
    HANDLE h = CreateFileW(tmpPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    if (!MoveFileExW(tmpPath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DWORD le = GetLastError();
        err = L"MoveFileExW failed. GetLastError=" + std::to_wstring(le);
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    return true;
}
static std::wstring prettyJson(const std::wstring& minified, int indentSpaces = 2)
{
    std::wstring out;
    out.reserve(minified.size() + minified.size() / 4);
    int indent = 0;
    bool inString = false;
    bool escape = false;
    auto newlineIndent = [&]()
    {
        out.push_back(L'\n');
        const size_t indentCount =
            (indent > 0 && indentSpaces > 0) ? (static_cast<size_t>(indent) * static_cast<size_t>(indentSpaces)) : 0u;
        out.append(indentCount, L' ');
    };
    for (wchar_t ch : minified)
    {
        if (escape)
        {
            out.push_back(ch);
            escape = false;
            continue;
        }
        if (ch == L'\\' && inString)
        {
            out.push_back(ch);
            escape = true;
            continue;
        }
        if (ch == L'"')
        {
            out.push_back(ch);
            inString = !inString;
            continue;
        }
        if (inString)
        {
            out.push_back(ch);
            continue;
        }
        switch (ch)
        {
            case L'{':
            case L'[':
                out.push_back(ch);
                indent++;
                newlineIndent();
                break;
            case L'}':
            case L']':
                indent--;
                newlineIndent();
                out.push_back(ch);
                break;
            case L',':
                out.push_back(ch);
                newlineIndent();
                break;
            case L':':
                out.push_back(ch);
                out.push_back(L' ');
                break;
            default:
                if (ch != L' ' && ch != L'\n' && ch != L'\r' && ch != L'\t')
                    out.push_back(ch);
                break;
        }
    }
    out.push_back(L'\n');
    return out;
}
const wchar_t* ThemeTagToString(ThemeTag t)
{
    switch (t)
    {
        case ThemeTag::Light:
            return L"light";
        case ThemeTag::Dark:
            return L"dark";
        case ThemeTag::Both:
            return L"both";
        case ThemeTag::Ignore:
            return L"ignore";
        default:
            return L"unknown";
    }
}
ThemeTag ThemeTagFromString(const std::wstring& s)
{
    auto v = toLower(s);
    if (v == L"light")
        return ThemeTag::Light;
    if (v == L"dark")
        return ThemeTag::Dark;
    if (v == L"both")
        return ThemeTag::Both;
    if (v == L"ignore")
        return ThemeTag::Ignore;
    return ThemeTag::Unknown;
}
static JsonValue wrapObjectValue(JsonObject const& obj)
{
    return JsonValue::Parse(obj.Stringify());
}
static JsonValue wrapArrayValue(JsonArray const& arr)
{
    return JsonValue::Parse(arr.Stringify());
}
static bool jsonTryGetObject(JsonObject const& obj, const std::wstring& key, JsonObject& out)
{
    if (!obj.HasKey(key))
        return false;
    auto v = obj.GetNamedValue(key);
    if (v.ValueType() != JsonValueType::Object)
        return false;
    out = v.GetObject();
    return true;
}
static bool jsonTryGetArray(JsonObject const& obj, const std::wstring& key, JsonArray& out)
{
    if (!obj.HasKey(key))
        return false;
    auto v = obj.GetNamedValue(key);
    if (v.ValueType() != JsonValueType::Array)
        return false;
    out = v.GetArray();
    return true;
}
static bool jsonTryGetString(JsonObject const& obj, const std::wstring& key, std::wstring& out)
{
    if (!obj.HasKey(key))
        return false;
    auto v = obj.GetNamedValue(key);
    if (v.ValueType() != JsonValueType::String)
        return false;
    out = v.GetString().c_str();
    return true;
}

struct WallpaperAlignmentSettings
{
    bool custom = false;
    int mode = 0;
    double position = 50.0;
    double x = 50.0;
    double y = 50.0;
    double z = 100.0;
    bool flipH = false;
};

struct WallpaperPlacement
{
    double displayW = 0.0;
    double displayH = 0.0;
    double sourceW = 0.0;
    double sourceH = 0.0;
    double contentX = 0.0;
    double contentY = 0.0;
    double contentW = 0.0;
    double contentH = 0.0;
    bool stretch = false;
    bool flipH = false;
};

static bool jsonTryGetBool(JsonObject const& obj, const std::wstring& key, bool& out)
{
    if (!obj.HasKey(key))
        return false;
    auto v = obj.GetNamedValue(key);
    if (v.ValueType() != JsonValueType::Boolean)
        return false;
    out = v.GetBoolean();
    return true;
}

static WallpaperAlignmentSettings readWallpaperAlignment(JsonObject const& monitor0)
{
    WallpaperAlignmentSettings a;
    double n = 0.0;

    if (jsonTryGetNumber(monitor0, L"alignment", n))
    {
        a.mode = static_cast<int>(n);
        a.custom = true;
    }
    const bool hasPosition = jsonTryGetNumber(monitor0, L"alignmentposition", a.position);
    const bool hasX = jsonTryGetNumber(monitor0, L"alignmentx", a.x);
    const bool hasY = jsonTryGetNumber(monitor0, L"alignmenty", a.y);
    const bool hasZ = jsonTryGetNumber(monitor0, L"alignmentz", a.z);
    const bool hasFlip = jsonTryGetBool(monitor0, L"alignmentfliph", a.flipH);

    if (hasPosition || hasX || hasY || hasZ || hasFlip)
        a.custom = true;

    if (!monitor0.HasKey(L"alignment") && (hasX || hasY || hasZ || hasFlip) && !hasPosition)
        a.mode = 4;

    a.position = (std::max)(0.0, (std::min)(100.0, a.position));
    a.x = (std::max)(0.0, (std::min)(100.0, a.x));
    a.y = (std::max)(0.0, (std::min)(100.0, a.y));
    a.z = (std::max)(1.0, (std::min)(400.0, a.z));
    return a;
}

static void getPrimaryDisplaySize(double fallbackW, double fallbackH, double& outW, double& outH)
{
    outW = static_cast<double>(GetSystemMetrics(SM_CXSCREEN));
    outH = static_cast<double>(GetSystemMetrics(SM_CYSCREEN));
    if (outW <= 0.0 || outH <= 0.0)
    {
        outW = fallbackW;
        outH = fallbackH;
    }
}

static WallpaperPlacement makeWallpaperPlacement(double sourceW, double sourceH, const WallpaperAlignmentSettings& align)
{
    WallpaperPlacement p;
    p.sourceW = sourceW;
    p.sourceH = sourceH;
    if (sourceW <= 0.0 || sourceH <= 0.0)
        return p;

    if (align.custom)
        getPrimaryDisplaySize(sourceW, sourceH, p.displayW, p.displayH);
    else
    {
        p.displayW = sourceW;
        p.displayH = sourceH;
    }

    // Wallpaper Engine 的 alignmentz 越小表示越靠近/放大；按倒数换成实际缩放倍率。
    const double zoom = 100.0 / align.z;
    const double scaleCover = (std::max)(p.displayW / sourceW, p.displayH / sourceH);
    const double scaleContain = (std::min)(p.displayW / sourceW, p.displayH / sourceH);
    const double pos = align.position / 100.0;

    p.flipH = align.flipH;
    switch (align.mode)
    {
        case 1: // Center
            p.contentW = sourceW * zoom;
            p.contentH = sourceH * zoom;
            p.contentX = (p.displayW - p.contentW) * 0.5;
            p.contentY = (p.displayH - p.contentH) * 0.5;
            break;
        case 2: // Stretch
            p.stretch = true;
            p.contentW = p.displayW;
            p.contentH = p.displayH;
            break;
        case 3: // Fill / fit inside
            p.contentW = sourceW * scaleContain * zoom;
            p.contentH = sourceH * scaleContain * zoom;
            p.contentX = (p.displayW - p.contentW) * pos;
            p.contentY = (p.displayH - p.contentH) * pos;
            break;
        case 4: // Free
            p.contentW = sourceW * scaleCover * zoom;
            p.contentH = sourceH * scaleCover * zoom;
            p.contentX = (p.displayW - p.contentW) * (align.x / 100.0);
            p.contentY = (p.displayH - p.contentH) * (align.y / 100.0);
            break;
        case 0: // Cover
        default:
            p.contentW = sourceW * scaleCover * zoom;
            p.contentH = sourceH * scaleCover * zoom;
            p.contentX = (p.displayW - p.contentW) * pos;
            p.contentY = (p.displayH - p.contentH) * pos;
            break;
    }

    if (p.contentW <= 0.0 || p.contentH <= 0.0)
    {
        p.contentW = sourceW;
        p.contentH = sourceH;
    }
    return p;
}

static bool mapDisplayToSource(const WallpaperPlacement& p, double displayX, double displayY, double& outX, double& outY)
{
    if (p.sourceW <= 0.0 || p.sourceH <= 0.0 || p.displayW <= 0.0 || p.displayH <= 0.0)
        return false;

    if (p.stretch)
    {
        outX = (displayX / p.displayW) * p.sourceW;
        outY = (displayY / p.displayH) * p.sourceH;
    }
    else
    {
        if (p.contentW <= 0.0 || p.contentH <= 0.0)
            return false;
        outX = ((displayX - p.contentX) / p.contentW) * p.sourceW;
        outY = ((displayY - p.contentY) / p.contentH) * p.sourceH;
    }

    if (p.flipH)
        outX = p.sourceW - outX;

    return outX >= 0.0 && outY >= 0.0 && outX < p.sourceW && outY < p.sourceH;
}

static std::wstring getCurrentUserName()
{
    wchar_t name[256] = {};
    DWORD size = static_cast<DWORD>(sizeof(name) / sizeof(name[0]));
    if (!GetUserNameW(name, &size) || size == 0)
        return L"";
    return name;
}
static bool tryGetSelectedWallpaperFile(JsonObject const& general, std::wstring& outFile)
{
    JsonObject wc, sel, mon;
    return jsonTryGetObject(general, L"wallpaperconfig", wc) && jsonTryGetObject(wc, L"selectedwallpapers", sel) &&
           jsonTryGetObject(sel, L"Monitor0", mon) && jsonTryGetString(mon, L"file", outFile) && !outFile.empty();
}
static bool isProfileObject(JsonObject const& candidate, JsonObject& outGeneral, JsonObject& outWprops)
{
    return jsonTryGetObject(candidate, L"general", outGeneral) && jsonTryGetObject(candidate, L"wproperties", outWprops);
}
static bool tryGetExplicitProfileKey(JsonObject const& root, std::wstring& out)
{
    static constexpr const wchar_t* kProfileKeys[] = {
        L"?selectedprofile", L"?selectedprofilekey", L"?activeprofile", L"?currentprofile",
        L"selectedprofile",  L"activeprofile",       L"currentprofile",  L"profile"};

    for (const wchar_t* key : kProfileKeys)
    {
        std::wstring candidateName;
        if (!jsonTryGetString(root, key, candidateName) || candidateName.empty())
            continue;

        for (auto const& kv : root)
        {
            if (toLower(kv.Key().c_str()) == toLower(candidateName))
            {
                auto v = kv.Value();
                if (v.ValueType() != JsonValueType::Object)
                    break;

                JsonObject general, wprops;
                if (isProfileObject(v.GetObject(), general, wprops))
                {
                    out = kv.Key().c_str();
                    return true;
                }
            }
        }
    }
    return false;
}
static std::wstring detectProfileKey(JsonObject const& root)
{
    std::wstring explicitProfileKey;
    tryGetExplicitProfileKey(root, explicitProfileKey);

    const std::wstring explicitProfileKeyLower = toLower(explicitProfileKey);
    const std::wstring currentUserLower = toLower(getCurrentUserName());

    std::wstring bestKey;
    int bestScore = -1;
    uint32_t bestOrder = UINT32_MAX;
    uint32_t order = 0;

    for (auto const& kv : root)
    {
        ++order;
        auto v = kv.Value();
        if (v.ValueType() != JsonValueType::Object)
            continue;
        JsonObject candidate = v.GetObject();
        JsonObject general, wprops;
        if (!isProfileObject(candidate, general, wprops))
            continue;

        const std::wstring key = kv.Key().c_str();
        const std::wstring keyLower = toLower(key);
        int score = 100;

        if (!explicitProfileKeyLower.empty() && keyLower == explicitProfileKeyLower)
            score += 100000;
        if (!currentUserLower.empty() && keyLower == currentUserLower)
            score += 50000;

        std::wstring activeFile;
        if (tryGetSelectedWallpaperFile(general, activeFile))
        {
            score += 5000;

            std::wstring exactKey;
            if (findObjectPathKey(wprops, activeFile, exactKey))
                score += 5000;
        }

        JsonArray playlists;
        if (jsonTryGetArray(general, L"playlists", playlists))
            score += 1000 + static_cast<int>((std::min)(playlists.Size(), 100u));

        score += static_cast<int>((std::min)(wprops.Size(), 100u));

        if (score > bestScore || (score == bestScore && order < bestOrder))
        {
            bestScore = score;
            bestOrder = order;
            bestKey = key;
        }
    }
    return bestKey;
}

static const double* GetSRGBLut()
{
    static double lut[256];
    static bool init = false;
    if (!init)
    {
        for (int i = 0; i < 256; ++i)
        {
            double c = i / 255.0;
            lut[i] = (c <= 0.04045) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
        }
        init = true;
    }
    return lut;
}

// ============================================================
// 【新增】生成无污染的纯净同步备份 (安全遍历版，修复崩溃地雷)
// ============================================================
static void backupCleanConfig(const ApplyOptions& opt, const JsonObject& originalRoot)
{
    std::wcout << L"\n  [备份防护] 正在生成纯净版 config.json.bak..." << std::endl;
    try
    {
        // 1. 深拷贝当前的 JSON，绝对不影响原有的 root 内存对象
        JsonObject clone;
        if (!JsonObject::TryParse(originalRoot.Stringify(), clone))
        {
            std::wcout << L"  -> [失败] JSON深拷贝解析异常。" << std::endl;
            return;
        }

        std::wstring profileKey = detectProfileKey(clone);
        if (!profileKey.empty())
        {
            JsonObject profileObj = clone.GetNamedValue(profileKey).GetObject();

            // ==========================================
            // 净化任务 A：剥离壁纸的自定义标签
            // ==========================================
            JsonObject wprops;
            if (jsonTryGetObject(profileObj, L"wproperties", wprops))
            {
                // 【修复核心】先收集所有 Key，绝不在迭代器中直接修改集合！
                std::vector<std::wstring> keys;
                for (auto const& kv : wprops)
                {
                    keys.push_back(kv.Key().c_str());
                }

                for (const auto& key : keys)
                {
                    auto val = wprops.GetNamedValue(key);
                    if (val.ValueType() == JsonValueType::Object)
                    {
                        JsonObject entryObj = val.GetObject();
                        JsonObject monitor0;
                        if (jsonTryGetObject(entryObj, L"Monitor0", monitor0))
                        {
                            bool modified = false;
                            if (monitor0.HasKey(L"sts_theme"))
                            {
                                monitor0.Remove(L"sts_theme");
                                modified = true;
                            }
                            if (monitor0.HasKey(L"sts_user_override"))
                            {
                                monitor0.Remove(L"sts_user_override");
                                modified = true;
                            }
                            if (modified)
                            {
                                entryObj.SetNamedValue(L"Monitor0", wrapObjectValue(monitor0));
                                wprops.SetNamedValue(key, wrapObjectValue(entryObj));
                            }
                        }
                    }
                }
                profileObj.SetNamedValue(L"wproperties", wrapObjectValue(wprops));
            }

            // ==========================================
            // 净化任务 B：抹除我们生成的播放列表
            // ==========================================
            JsonObject general;
            if (jsonTryGetObject(profileObj, L"general", general))
            {
                std::wstring lightAuto = opt.lightAutoPlaylistName.empty() ? L"White_auto" : opt.lightAutoPlaylistName;
                std::wstring darkAuto = opt.darkAutoPlaylistName.empty() ? L"Black_auto" : opt.darkAutoPlaylistName;

                // B1: 从播放列表大纲中删除
                JsonArray playlists;
                if (jsonTryGetArray(general, L"playlists", playlists))
                {
                    for (int i = static_cast<int>(playlists.Size()) - 1; i >= 0; --i)
                    {
                        auto v = playlists.GetAt(static_cast<uint32_t>(i));
                        if (v.ValueType() == JsonValueType::Object)
                        {
                            std::wstring pName;
                            if (jsonTryGetString(v.GetObject(), L"name", pName))
                            {
                                if (pName == lightAuto || pName == darkAuto)
                                {
                                    playlists.RemoveAt(static_cast<uint32_t>(i));
                                }
                            }
                        }
                    }
                    general.SetNamedValue(L"playlists", wrapArrayValue(playlists));
                }

                // B2: 卸载桌面正在使用的轮换状态
                JsonObject wc, sel, mon;
                if (jsonTryGetObject(general, L"wallpaperconfig", wc) &&
                    jsonTryGetObject(wc, L"selectedwallpapers", sel) && jsonTryGetObject(sel, L"Monitor0", mon))
                {
                    if (mon.HasKey(L"playlist"))
                    {
                        auto pv = mon.GetNamedValue(L"playlist");
                        if (pv.ValueType() == JsonValueType::Object)
                        {
                            std::wstring currentName;
                            if (jsonTryGetString(pv.GetObject(), L"name", currentName))
                            {
                                if (currentName == lightAuto || currentName == darkAuto)
                                {
                                    mon.Remove(L"playlist");
                                    sel.SetNamedValue(L"Monitor0", wrapObjectValue(mon));
                                    wc.SetNamedValue(L"selectedwallpapers", wrapObjectValue(sel));
                                    general.SetNamedValue(L"wallpaperconfig", wrapObjectValue(wc));
                                }
                            }
                        }
                    }
                }
                profileObj.SetNamedValue(L"general", wrapObjectValue(general));
            }

            clone.SetNamedValue(profileKey, wrapObjectValue(profileObj));
        }

        // 3. 写入同目录的 config.json.bak 文件
        std::wstring bakPath = opt.configPath + L".bak";
        std::string outText = utf16ToUtf8(prettyJson(clone.Stringify().c_str(), 2));
        if (writeAllTextUtf8(bakPath, outText))
        {
            if (opt.printDiagnostics)
                std::wcout << L"  -> [成功] 内容已同步，且私有标签与列表已洗脱！" << std::endl;
        }
        else
        {
            if (opt.printDiagnostics)
                std::wcout << L"  -> [失败] 无法写入 .bak 文件，请检查权限。" << std::endl;
        }
    }
    catch (...)
    {
        if (opt.printDiagnostics)
            std::wcout << L"  -> [失败] 备份过程发生未知异常。" << std::endl;
    }
}

// ============================================================
// 视觉核心：运算规则与色彩判定
// ============================================================

static ClassifyResult ClassifyByStats(const ClassifyFeatures& f)
{
    ThemeTag tag = ThemeTag::Unknown;
    double conf = 0.50;

    if (f.globalAvg >= 0.48)
    {
        if (f.roiAvg <= 0.22 && f.roiDarkRatio >= 0.60 && f.globalDarkRatio >= 0.22)
        { tag = ThemeTag::Dark; conf = 0.82;
            if (f.roiAvg <= 0.18 && f.roiDarkRatio >= 0.65) { tag = ThemeTag::Ignore; conf = 0.85; } }
        else { tag = ThemeTag::Light; conf = 0.90; }
    }
    else if (f.globalDarkRatio >= 0.50 && f.globalAvg <= 0.30 && (f.roiDarkRatio >= 0.22 || f.roiAvg <= 0.34))
    { tag = ThemeTag::Dark; conf = 0.88; }
    else if (f.globalAvg >= 0.22 && f.globalAvg <= 0.40 && f.globalDarkRatio >= 0.35 && f.globalDarkRatio <= 0.55 &&
             f.roiAvg >= 0.35 && f.roiDarkRatio <= 0.25)
    { tag = ThemeTag::Both; conf = 0.75; }
    else if (f.globalAvg >= 0.28 && f.roiAvg >= 0.26 && f.roiDarkRatio <= 0.35)
    {
        if (f.globalDarkRatio >= 0.42 && f.globalAvg <= 0.35) { tag = ThemeTag::Both; conf = 0.72; }
        else { tag = ThemeTag::Light; conf = 0.82; }
    }
    else if (f.globalAvg >= 0.25 && f.globalAvg <= 0.42 && f.roiAvg >= 0.16 && f.roiAvg <= 0.29 &&
             f.roiDarkRatio <= 0.55)
    { tag = ThemeTag::Both; conf = 0.68; }
    else if (f.roiDarkRatio >= 0.68 && f.roiAvg < 0.25)
    { tag = ThemeTag::Dark; conf = 0.62; }
    else if (f.roiAvg >= 0.16)
    {
        if (f.globalDarkRatio >= 0.48 && f.globalAvg <= 0.30) { tag = ThemeTag::Both; conf = 0.70; }
        else { tag = ThemeTag::Light; conf = 0.60; }
    }
    else if (f.roiAvg <= 0.10)
    { tag = ThemeTag::Dark; conf = 0.55; }
    else
    { tag = ThemeTag::Both; conf = 0.50; }

    // No Both: resolve to Light or Dark
    if (tag == ThemeTag::Both)
        tag = (f.globalAvg >= 0.44) ? ThemeTag::Light : ThemeTag::Dark;

    // Tray readability: globally bright with mixed tray pixels -> Dark
    if (tag == ThemeTag::Light && f.roiDarkRatio > 0.40)
        tag = ThemeTag::Dark;

    // Borderline Dark with moderate global darkness -> Both (then Light)
    if (tag == ThemeTag::Dark && f.globalDarkRatio < 0.45 && f.roiAvg > 0.10 && f.roiDarkRatio < 0.70)
        tag = ThemeTag::Both;

    if (conf < 0.0) conf = 0.0;
    if (conf > 1.0) conf = 1.0;
    return {tag, conf};
}

static double rgbToLinearLuminance(uint8_t r, uint8_t g, uint8_t b)
{
    const double* lut = GetSRGBLut();
    return 0.2126 * lut[r] + 0.7152 * lut[g] + 0.0722 * lut[b];
}

static bool calcRgbaRoiStatsAligned(const PkgParser::RgbaImage& img, double wPct, double hPct,
                                    const WallpaperAlignmentSettings& alignment, double& outRoiAvg,
                                    double& outRoiDark, double& outGlobalAvg, double& outGlobalDark)
{
    if (!img.IsValid() || img.width <= 0 || img.height <= 0)
        return false;

    const int sourceW = (img.imageWidth > 0) ? img.imageWidth : img.width;
    const int sourceH = (img.imageHeight > 0) ? img.imageHeight : img.height;
    if (sourceW <= 0 || sourceH <= 0)
        return false;

    const WallpaperPlacement placement = makeWallpaperPlacement(sourceW, sourceH, alignment);
    const int displayW = (std::max)(1, static_cast<int>(placement.displayW + 0.5));
    const int displayH = (std::max)(1, static_cast<int>(placement.displayH + 0.5));
    const int roiW = (std::max)(1, static_cast<int>(placement.displayW * wPct));
    const int roiH = (std::max)(1, static_cast<int>(placement.displayH * hPct));
    const int roiX = (std::max)(0, displayW - roiW);
    const int roiY = (std::max)(0, displayH - roiH);
    const int step = alignment.custom ? 4 : 1;

    double sumGlobalL = 0.0;
    double sumRoiL = 0.0;
    int darkGlobal = 0;
    int darkRoi = 0;
    int globalCount = 0;
    int roiCount = 0;

    for (int y = 0; y < sourceH; y += 4)
    {
        const uint8_t* row = img.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(img.width) * 4u;
        for (int x = 0; x < sourceW; x += 4)
        {
            const uint8_t* px = row + static_cast<size_t>(x) * 4u;
            const double alpha = px[3] / 255.0;
            const uint8_t r = static_cast<uint8_t>((std::min)(255.0, px[0] * alpha + 0.5));
            const uint8_t g = static_cast<uint8_t>((std::min)(255.0, px[1] * alpha + 0.5));
            const uint8_t b = static_cast<uint8_t>((std::min)(255.0, px[2] * alpha + 0.5));
            const double L = rgbToLinearLuminance(r, g, b);
            sumGlobalL += L;
            ++globalCount;
            if (L < 0.179)
                ++darkGlobal;
        }
    }

    for (int dy = roiY; dy < displayH; dy += step)
    {
        for (int dx = roiX; dx < displayW; dx += step)
        {
            double sx = 0.0, sy = 0.0;
            double L = 0.0;
            if (mapDisplayToSource(placement, dx + 0.5, dy + 0.5, sx, sy))
            {
                const int tx = (std::max)(0, (std::min)(sourceW - 1, static_cast<int>(sx)));
                const int ty = (std::max)(0, (std::min)(sourceH - 1, static_cast<int>(sy)));
                const uint8_t* px =
                    img.pixels.data() + (static_cast<size_t>(ty) * static_cast<size_t>(img.width) + tx) * 4u;
                const double alpha = px[3] / 255.0;
                const uint8_t r = static_cast<uint8_t>((std::min)(255.0, px[0] * alpha + 0.5));
                const uint8_t g = static_cast<uint8_t>((std::min)(255.0, px[1] * alpha + 0.5));
                const uint8_t b = static_cast<uint8_t>((std::min)(255.0, px[2] * alpha + 0.5));
                L = rgbToLinearLuminance(r, g, b);
            }

            sumRoiL += L;
            ++roiCount;
            if (L < 0.179)
                ++darkRoi;
        }
    }

    if (globalCount == 0 || roiCount == 0)
        return false;
    outGlobalAvg = sumGlobalL / static_cast<double>(globalCount);
    outGlobalDark = static_cast<double>(darkGlobal) / static_cast<double>(globalCount);
    outRoiAvg = sumRoiL / static_cast<double>(roiCount);
    outRoiDark = static_cast<double>(darkRoi) / static_cast<double>(roiCount);
    return true;
}

// ============================================================
// WIC 图像处理
// ============================================================
static bool calcImageRoiStatsWIC(const std::wstring& imagePath, double wPct, double hPct, double& outAvgLuminance,
                                 double& outDarkRatio)
{
    winrt::com_ptr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(imagePath.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, decoder.put())))
        return false;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put())))
        return false;
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0)
        return false;
    WICRect rect{};
    rect.Width = (std::max)(1u, static_cast<UINT>(w * wPct));
    rect.Height = (std::max)(1u, static_cast<UINT>(h * hPct));
    rect.X = w - rect.Width;
    rect.Y = h - rect.Height;
    winrt::com_ptr<IWICBitmapClipper> clipper;
    if (FAILED(factory->CreateBitmapClipper(clipper.put())))
        return false;
    if (FAILED(clipper->Initialize(frame.get(), &rect)))
        return false;
    winrt::com_ptr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.put())))
        return false;
    if (FAILED(converter->Initialize(clipper.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeCustom)))
        return false;
    const size_t pixelCount = static_cast<size_t>(rect.Width) * static_cast<size_t>(rect.Height);
    std::vector<BYTE> pixels(pixelCount * 4u);
    if (FAILED(converter->CopyPixels(nullptr, rect.Width * 4, static_cast<UINT>(pixels.size()), pixels.data())))
        return false;
    const double* lut = GetSRGBLut();
    double sumLum = 0.0;
    int darkPixels = 0;
    const size_t count = pixelCount;
    for (size_t i = 0; i < count; ++i)
    {
        const size_t offset = i * 4u;
        BYTE b = pixels[offset + 0], g = pixels[offset + 1], r = pixels[offset + 2];
        double L = 0.2126 * lut[r] + 0.7152 * lut[g] + 0.0722 * lut[b];
        sumLum += L;
        if (L < 0.179)
            darkPixels++;
    }
    outAvgLuminance = sumLum / static_cast<double>(count);
    outDarkRatio = static_cast<double>(darkPixels) / static_cast<double>(count);
    return true;
}

static bool calcImageRoiStatsWICAligned(const std::wstring& imagePath, double wPct, double hPct,
                                        const WallpaperAlignmentSettings& alignment, double& outAvgLuminance,
                                        double& outDarkRatio)
{
    if (!alignment.custom)
        return calcImageRoiStatsWIC(imagePath, wPct, hPct, outAvgLuminance, outDarkRatio);

    winrt::com_ptr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(imagePath.c_str(), nullptr, GENERIC_READ,
                                                  WICDecodeMetadataCacheOnDemand, decoder.put())))
        return false;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put())))
        return false;
    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    if (w == 0 || h == 0)
        return false;

    winrt::com_ptr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.put())))
        return false;
    if (FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeCustom)))
        return false;

    const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
    std::vector<BYTE> pixels(pixelCount * 4u);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()), pixels.data())))
        return false;

    const WallpaperPlacement placement = makeWallpaperPlacement(static_cast<double>(w), static_cast<double>(h), alignment);
    const int displayW = (std::max)(1, static_cast<int>(placement.displayW + 0.5));
    const int displayH = (std::max)(1, static_cast<int>(placement.displayH + 0.5));
    const int roiW = (std::max)(1, static_cast<int>(placement.displayW * wPct));
    const int roiH = (std::max)(1, static_cast<int>(placement.displayH * hPct));
    const int roiX = (std::max)(0, displayW - roiW);
    const int roiY = (std::max)(0, displayH - roiH);

    double sumLum = 0.0;
    int darkPixels = 0;
    int count = 0;
    for (int dy = roiY; dy < displayH; dy += 4)
    {
        for (int dx = roiX; dx < displayW; dx += 4)
        {
            double sx = 0.0, sy = 0.0;
            double L = 0.0;
            if (mapDisplayToSource(placement, dx + 0.5, dy + 0.5, sx, sy))
            {
                const int tx = (std::max)(0, (std::min)(static_cast<int>(w) - 1, static_cast<int>(sx)));
                const int ty = (std::max)(0, (std::min)(static_cast<int>(h) - 1, static_cast<int>(sy)));
                const size_t offset = (static_cast<size_t>(ty) * static_cast<size_t>(w) + tx) * 4u;
                L = rgbToLinearLuminance(pixels[offset + 2], pixels[offset + 1], pixels[offset + 0]);
            }
            sumLum += L;
            ++count;
            if (L < 0.179)
                ++darkPixels;
        }
    }

    if (count == 0)
        return false;
    outAvgLuminance = sumLum / static_cast<double>(count);
    outDarkRatio = static_cast<double>(darkPixels) / static_cast<double>(count);
    return true;
}

static bool calcVideoRoiStatsMF(const std::wstring& videoPath, double wPct, double hPct,
                                const WallpaperAlignmentSettings& alignment, double& outRoiAvg, double& outRoiDark,
                                double& outGlobalAvg, double& outGlobalDark)
{
    // 配置 Source Reader 启用底层视频处理（用于格式转换）
    winrt::com_ptr<IMFAttributes> pAttributes;
    if (FAILED(MFCreateAttributes(pAttributes.put(), 1)))
    {
        return false;
    }
    pAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    // 【终极内存防线 3】禁止 MF 引擎为了流畅播放而在底层疯狂开辟内存池预读缓冲！
    pAttributes->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE);
    winrt::com_ptr<IMFSourceReader> pReader;
    if (FAILED(MFCreateSourceReaderFromURL(videoPath.c_str(), pAttributes.get(), pReader.put())))
    {
        return false;
    }

    // 获取视频总时长 (单位：100纳秒)
    PROPVARIANT var;
    PropVariantInit(&var);
    LONGLONG duration = 0;
    if (SUCCEEDED(pReader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var)))
    {
        if (var.vt == VT_UI8)
            duration = var.uhVal.QuadPart;
    }
    PropVariantClear(&var);

    // 动态抽帧防黑屏策略：<3秒取50%，>=3秒取20%
    LONGLONG seekTime = 0;
    if (duration > 0)
    {
        if (duration < 30000000LL) // 3秒 = 30,000,000
            seekTime = duration / 2;
        else
            seekTime = duration / 5;
    }

    // 强制要求输出格式为 RGB32 裸数据
    winrt::com_ptr<IMFMediaType> pMediaType;
    MFCreateMediaType(pMediaType.put());
    pMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pMediaType.get())))
    {
        return false;
    }

    // 精准 Seek 到计算好的安全时间戳
    var.vt = VT_I8;
    var.hVal.QuadPart = seekTime;
    pReader->SetCurrentPosition(GUID_NULL, var);

    // 抽取该帧
    winrt::com_ptr<IMFSample> pSample;
    DWORD streamIndex, flags;
    LONGLONG llTimeStamp;
    if (FAILED(pReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &llTimeStamp,
                                   pSample.put())) ||
        !pSample)
    {
        return false;
    }

    // 获取当前媒体类型以提取画面宽高
    winrt::com_ptr<IMFMediaType> pCurrentType;
    if (FAILED(pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, pCurrentType.put())))
    {
        return false;
    }
    UINT32 width = 0, height = 0;
    MFGetAttributeSize(pCurrentType.get(), MF_MT_FRAME_SIZE, &width, &height);
    if (width == 0 || height == 0)
    {
        return false;
    }

    // 将 Sample 转换为连续的内存 Buffer
    winrt::com_ptr<IMFMediaBuffer> pBuffer;
    if (FAILED(pSample->ConvertToContiguousBuffer(pBuffer.put())))
    {
        return false;
    }

    BYTE* pData = nullptr;
    DWORD maxLength = 0, currentLength = 0;
    if (FAILED(pBuffer->Lock(&pData, &maxLength, &currentLength)))
    {
        return false;
    }

    // 获取内存步长 (Stride)。负数代表画面是 Bottom-up (自底向上) 存储的
    LONG stride = static_cast<LONG>(static_cast<unsigned long long>(width) * 4ull);
    pCurrentType->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32*)&stride);
    bool isBottomUp = (stride < 0);
    LONG absStride = std::abs(stride);

    const WallpaperPlacement placement =
        makeWallpaperPlacement(static_cast<double>(width), static_cast<double>(height), alignment);
    const int displayW = (std::max)(1, static_cast<int>(placement.displayW + 0.5));
    const int displayH = (std::max)(1, static_cast<int>(placement.displayH + 0.5));
    const int roiW = (std::max)(1, static_cast<int>(placement.displayW * wPct));
    const int roiH = (std::max)(1, static_cast<int>(placement.displayH * hPct));
    const int roiX = (std::max)(0, displayW - roiW);
    const int roiY = (std::max)(0, displayH - roiH);
    const int roiStep = alignment.custom ? 4 : 1;

    const double* lut = GetSRGBLut();
    double sumGlobalL = 0.0, sumRoiL = 0.0;
    int darkGlobal = 0, darkRoi = 0;
    int roiCount = 0;

    // 遍历像素计算亮度和暗比 (RGB32 内存中通常是 B, G, R, A 排列)
    for (UINT32 y = 0; y < height; ++y)
    {
        BYTE* row = pData + static_cast<size_t>(y) * static_cast<size_t>(absStride);

        for (UINT32 x = 0; x < width; ++x)
        {
            BYTE b = row[x * 4 + 0];
            BYTE g = row[x * 4 + 1];
            BYTE r = row[x * 4 + 2];

            double L = 0.2126 * lut[r] + 0.7152 * lut[g] + 0.0722 * lut[b];

            // 全局统计
            sumGlobalL += L;
            if (L < 0.179)
                darkGlobal++;
        }
    }

    for (int dy = roiY; dy < displayH; dy += roiStep)
    {
        for (int dx = roiX; dx < displayW; dx += roiStep)
        {
            double sx = 0.0, sy = 0.0;
            double L = 0.0;
            if (mapDisplayToSource(placement, dx + 0.5, dy + 0.5, sx, sy))
            {
                const UINT32 tx = (std::max)(0u, (std::min)(width - 1u, static_cast<UINT32>(sx)));
                const UINT32 ty = (std::max)(0u, (std::min)(height - 1u, static_cast<UINT32>(sy)));
                const UINT32 memoryY = isBottomUp ? (height - 1u - ty) : ty;
                BYTE* row = pData + static_cast<size_t>(memoryY) * static_cast<size_t>(absStride);
                BYTE b = row[tx * 4 + 0];
                BYTE g = row[tx * 4 + 1];
                BYTE r = row[tx * 4 + 2];
                L = 0.2126 * lut[r] + 0.7152 * lut[g] + 0.0722 * lut[b];
            }
            sumRoiL += L;
            ++roiCount;
            if (L < 0.179)
                darkRoi++;
        }
    }

    // 数据回写
    outGlobalAvg = sumGlobalL / (width * height);
    outGlobalDark = static_cast<double>(darkGlobal) / (width * height);
    if (roiCount == 0)
    {
        pBuffer->Unlock();
        return false;
    }
    outRoiAvg = sumRoiL / static_cast<double>(roiCount);
    outRoiDark = static_cast<double>(darkRoi) / static_cast<double>(roiCount);

    // 解锁与清理
    // 【终极内存防线 4】主动解除内存锁定，强杀 COM 指针，不给系统留任何延迟释放的借口
    pBuffer->Unlock();
    pBuffer = nullptr;
    pSample = nullptr;
    pReader = nullptr;
    return true;
}

// ============================================================
// 静态打标 (Pass 1 & Pass 2)
// ============================================================
static bool tryGetProjectJsonSchemecolor(const std::wstring& pj, std::wstring& outScheme)
{
    std::string text;
    if (!readAllTextUtf8(pj, text))
        return false;
    JsonObject root;
    try
    {
        if (!JsonObject::TryParse(winrt::to_hstring(text), root))
            return false;
    }
    catch (...)
    {
        return false;
    }
    JsonObject gen;
    if (!jsonTryGetObject(root, L"general", gen))
        return false;
    if (jsonTryGetString(gen, L"schemecolor", outScheme))
        return true;

    JsonObject props;
    if (jsonTryGetObject(gen, L"properties", props))
    {
        JsonObject schemeObj;
        if (jsonTryGetObject(props, L"schemecolor", schemeObj) &&
            jsonTryGetString(schemeObj, L"value", outScheme))
            return true;
    }

    return false;
}

static bool tryGetMainWallpaperKeyFromProjectJson(const std::wstring& pj, std::wstring& outKey)
{
    std::string text;
    if (!readAllTextUtf8(pj, text))
        return false;
    JsonObject root;
    try
    {
        if (!JsonObject::TryParse(winrt::to_hstring(text), root))
            return false;
    }
    catch (...)
    {
        return false;
    }
    std::wstring file;
    if (jsonTryGetString(root, L"file", file))
    {
        std::wstring dir = getParentDir(pj);
        outKey = normalizeSlashes(dir) + L"/" + normalizeSlashes(file);
        return true;
    }
    return false;
}
static bool tryGetPreviewPathFromProjectJson(const std::wstring& projectJsonPath, std::wstring& outPreviewPath)
{
    std::string text;
    if (!readAllTextUtf8(projectJsonPath, text))
        return false;
    JsonObject root;
    try
    {
        if (!JsonObject::TryParse(winrt::to_hstring(text), root))
            return false;
    }
    catch (...)
    {
        return false;
    }
    std::wstring previewFile;
    if (jsonTryGetString(root, L"preview", previewFile))
    {
        outPreviewPath = normalizeSlashes(getParentDir(projectJsonPath)) + L"/" + normalizeSlashes(previewFile);
        return fileExists(outPreviewPath);
    }
    return false;
}

// ============================================================
// 静态打标 (Pass 1 & Pass 2)
// ============================================================
static ThemeTag evaluateL2TrayRoi(const std::wstring& wallpaperDir, const ApplyOptions& opt,
                                  const WallpaperAlignmentSettings& alignment)
{

    // 提取文件夹名称（即壁纸 ID）
    std::wstring wpId = wallpaperDir;
    auto slashPos = wpId.find_last_of(L'/');
    if (slashPos != std::wstring::npos)
        wpId = wpId.substr(slashPos + 1);

    // 原有逻辑：继续寻找并计算预览图
    std::wstring previewPath = joinPath(wallpaperDir, L"preview.jpg");
    if (!fileExists(previewPath))
    {
        previewPath = joinPath(wallpaperDir, L"preview.png");
        if (!fileExists(previewPath))
        {
            previewPath = joinPath(wallpaperDir, L"preview.gif");
            if (!fileExists(previewPath))
            {
                std::wstring pj = joinPath(wallpaperDir, L"project.json");
                if (!tryGetPreviewPathFromProjectJson(pj, previewPath))
                    return ThemeTag::Unknown;
            }
        }
    }

    double roiAvg = 0.0, roiDarkRatio = 0.0;
    if (!calcImageRoiStatsWICAligned(previewPath, opt.trayRoiWidthPct, opt.trayRoiHeightPct, alignment, roiAvg,
                                     roiDarkRatio))
        return ThemeTag::Unknown;

    double globalAvg = 0.0, globalDarkRatio = 0.0;
    calcImageRoiStatsWIC(previewPath, 1.0, 1.0, globalAvg, globalDarkRatio);

    { ClassifyFeatures _f; _f.roiAvg=roiAvg; _f.roiDarkRatio=roiDarkRatio; _f.globalAvg=globalAvg; _f.globalDarkRatio=globalDarkRatio; auto _cr=ClassifyByStats(_f); return _cr.tag; }
}

// 进程与其他辅助工具实现
static void sleepMs(unsigned ms)
{
    Sleep(ms);
}
static bool isProcessRunningByName(const wchar_t* exeName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, exeName) == 0)
            {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}
static bool isWallpaperEngineRunning()
{
    return isProcessRunningByName(L"wallpaper64.exe") || isProcessRunningByName(L"wallpaper32.exe");
}
static void killWallpaperEngineHard()
{
    ShellExecuteW(nullptr, L"open", L"taskkill", L"/IM wallpaper64.exe /T /F", nullptr, SW_HIDE);
    ShellExecuteW(nullptr, L"open", L"taskkill", L"/IM wallpaper32.exe /T /F", nullptr, SW_HIDE);
}
static bool waitProcessExitByName(unsigned timeoutMs)
{
    unsigned waited = 0;
    const unsigned step = 100;
    while (waited < timeoutMs)
    {
        if (!isWallpaperEngineRunning())
            return true;
        Sleep(step);
        waited += step;
    }
    return !isWallpaperEngineRunning();
}
static bool waitFileWritable(const std::wstring& path, unsigned timeoutMs)
{
    unsigned waited = 0;
    const unsigned step = 100;
    while (waited < timeoutMs)
    {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            CloseHandle(h);
            return true;
        }
        Sleep(step);
        waited += step;
    }
    return false;
}
static std::wstring inferWallpaperEngineExe(const std::wstring& installDir)
{
    if (installDir.empty())
        return L"";
    std::wstring base = installDir;
    if (base.back() != L'\\' && base.back() != L'/')
        base += L'\\';
    if (fileExists(base + L"wallpaper64.exe"))
        return base + L"wallpaper64.exe";
    if (fileExists(base + L"wallpaper32.exe"))
        return base + L"wallpaper32.exe";
    return L"";
}
static bool startWallpaperEngineExe(const std::wstring& exePath, std::wstring& err)
{
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"open";
    sei.lpFile = exePath.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei))
    {
        err = L"Failed to start Wallpaper Engine.";
        return false;
    }
    if (sei.hProcess)
        CloseHandle(sei.hProcess);
    return true;
}
static std::vector<DWORD> collectPidsByName(const wchar_t* exeName)
{
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe))
        {
            do
            {
                if (_wcsicmp(pe.szExeFile, exeName) == 0)
                    pids.push_back(pe.th32ProcessID);
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    return pids;
}
static void requestCloseProcessWindows(DWORD pid)
{
    struct CloseCtx
    {
        DWORD pid;
        int closedCount;
    };
    CloseCtx ctx{pid, 0};
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL
        {
            auto c = (CloseCtx*)lp;
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == c->pid)
            {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                c->closedCount++;
            }
            return TRUE;
        },
        (LPARAM)&ctx);
}
static bool tryGracefulExitWallpaperEngine(unsigned gracefulTimeoutMs)
{
    for (DWORD pid : collectPidsByName(L"wallpaper64.exe"))
        requestCloseProcessWindows(pid);
    for (DWORD pid : collectPidsByName(L"wallpaper32.exe"))
        requestCloseProcessWindows(pid);
    return waitProcessExitByName(gracefulTimeoutMs);
}

static int findPlaylistIndexByName(JsonArray const& playlists, const std::wstring& name)
{
    for (uint32_t i = 0; i < playlists.Size(); ++i)
    {
        auto v = playlists.GetAt(i);
        if (v.ValueType() != JsonValueType::Object)
            continue;
        std::wstring n;
        if (jsonTryGetString(v.GetObject(), L"name", n) && n == name)
            return static_cast<int>(i);
    }
    return -1;
}
static bool arrayContainsString(JsonArray const& arr, const std::wstring& s)
{
    for (uint32_t i = 0; i < arr.Size(); ++i)
    {
        auto v = arr.GetAt(i);
        if (v.ValueType() == JsonValueType::String && samePathKey(v.GetString().c_str(), s))
            return true;
    }
    return false;
}
static bool sameWallpaperIdentity(const std::wstring& a, const std::wstring& b)
{
    if (samePathKey(a, b))
        return true;

    const std::wstring aDir = canonicalPathCompareKey(getWallpaperDir(a));
    const std::wstring bDir = canonicalPathCompareKey(getWallpaperDir(b));
    return !aDir.empty() && aDir == bDir;
}
static bool removeWallpaperFromArray(JsonArray& arr, const std::wstring& s, bool keepExactMatch)
{
    bool removed = false;
    for (int i = static_cast<int>(arr.Size()) - 1; i >= 0; --i)
    {
        auto v = arr.GetAt(static_cast<uint32_t>(i));
        if (v.ValueType() != JsonValueType::String)
            continue;

        const std::wstring item = v.GetString().c_str();
        if (keepExactMatch && samePathKey(item, s))
            continue;

        if (sameWallpaperIdentity(item, s))
        {
            arr.RemoveAt(static_cast<uint32_t>(i));
            removed = true;
        }
    }
    return removed;
}
static bool removeStringFromArray(JsonArray& arr, const std::wstring& s)
{
    bool removed = false;
    for (int i = static_cast<int>(arr.Size()) - 1; i >= 0; --i)
    {
        auto v = arr.GetAt(static_cast<uint32_t>(i));
        if (v.ValueType() == JsonValueType::String && samePathKey(v.GetString().c_str(), s))
        {
            arr.RemoveAt(static_cast<uint32_t>(i));
            removed = true;
        }
    }
    return removed;
}
static bool addUniqueString(JsonArray& arr, const std::wstring& s)
{
    if (arrayContainsString(arr, s))
        return false;
    arr.Append(JsonValue::CreateStringValue(s));
    return true;
}
static bool removeStringFromOtherPlaylists(JsonArray& playlists, int lightIdx, int darkIdx, const std::wstring& s)
{
    bool removed = false;
    for (uint32_t i = 0; i < playlists.Size(); ++i)
    {
        if (static_cast<int>(i) == lightIdx || static_cast<int>(i) == darkIdx)
            continue;

        auto v = playlists.GetAt(i);
        if (v.ValueType() != JsonValueType::Object)
            continue;

        JsonObject playlist = v.GetObject();
        JsonArray items;
        if (!jsonTryGetArray(playlist, L"items", items))
            continue;

        if (removeWallpaperFromArray(items, s, false))
        {
            playlist.SetNamedValue(L"items", wrapArrayValue(items));
            playlists.SetAt(i, wrapObjectValue(playlist));
            removed = true;
        }
    }
    return removed;
}
static bool ensurePlaylist(JsonArray& playlists, const std::wstring& name, JsonObject const& settingsTemplate,
                           int& outIndex)
{
    int idx = findPlaylistIndexByName(playlists, name);
    if (idx >= 0)
    {
        outIndex = idx;
        return false;
    }
    JsonObject p;
    p.SetNamedValue(L"name", JsonValue::CreateStringValue(name));
    JsonArray items;
    p.SetNamedValue(L"items", wrapArrayValue(items));
    p.SetNamedValue(L"settings", wrapObjectValue(settingsTemplate));
    playlists.Append(wrapObjectValue(p));
    outIndex = static_cast<int>(playlists.Size() - 1);
    return true;
}
static void ensureMonitor0Object(JsonObject& entryObj, JsonObject& outMonitor0)
{
    if (entryObj.HasKey(L"Monitor0"))
    {
        auto v = entryObj.GetNamedValue(L"Monitor0");
        if (v.ValueType() == JsonValueType::Object)
        {
            outMonitor0 = v.GetObject();
            return;
        }
    }
    JsonObject m0;
    entryObj.SetNamedValue(L"Monitor0", wrapObjectValue(m0));
    outMonitor0 = m0;
}
static bool tryReadThemeTag(JsonObject const& monitor0, ThemeTag& out)
{
    std::wstring s;
    if (!jsonTryGetString(monitor0, L"sts_theme", s))
        return false;
    out = ThemeTagFromString(s);
    return out != ThemeTag::Unknown;
}
static bool writeThemeTag(JsonObject& monitor0, ThemeTag tag)
{
    if (tag == ThemeTag::Unknown)
        return false;
    monitor0.SetNamedValue(L"sts_theme", JsonValue::CreateStringValue(ThemeTagToString(tag)));
    return true;
}
static bool ensureAndGetPlaylists(JsonObject& general, JsonArray& outPlaylists, std::wstring& err)
{
    if (!general.HasKey(L"playlists"))
    {
        JsonArray playlists;
        general.SetNamedValue(L"playlists", wrapArrayValue(playlists));
        outPlaylists = playlists;
        return true;
    }
    auto v = general.GetNamedValue(L"playlists");
    if (v.ValueType() != JsonValueType::Array)
    {
        err = L"general.playlists is not an array.";
        return false;
    }
    outPlaylists = v.GetArray();
    return true;
}
static bool tryGetSelectedMonitor0(JsonObject const& general, JsonObject& outMonitor0)
{
    JsonObject wc, selected;
    return jsonTryGetObject(general, L"wallpaperconfig", wc) &&
           jsonTryGetObject(wc, L"selectedwallpapers", selected) &&
           jsonTryGetObject(selected, L"Monitor0", outMonitor0);
}
static bool tryGetActivePlaylist(JsonObject const& general, JsonArray const& playlists, JsonObject& outPlaylist,
                                 std::wstring& outName)
{
    JsonObject monitor0;
    if (!tryGetSelectedMonitor0(general, monitor0) || !monitor0.HasKey(L"playlist"))
        return false;

    auto v = monitor0.GetNamedValue(L"playlist");
    if (v.ValueType() != JsonValueType::Object)
        return false;

    JsonObject selectedPlaylist = v.GetObject();
    std::wstring name;
    if (jsonTryGetString(selectedPlaylist, L"name", name) && !name.empty())
    {
        const int idx = findPlaylistIndexByName(playlists, name);
        if (idx >= 0 && playlists.GetAt(static_cast<uint32_t>(idx)).ValueType() == JsonValueType::Object)
        {
            outName = name;
            outPlaylist = playlists.GetAt(static_cast<uint32_t>(idx)).GetObject();
            return true;
        }
    }

    outName = name;
    outPlaylist = selectedPlaylist;
    return true;
}
static bool playlistItemsFitTheme(JsonObject const& playlist, JsonArray const& allowedItems)
{
    JsonArray items;
    if (!jsonTryGetArray(playlist, L"items", items) || items.Size() == 0)
        return false;

    bool hasWallpaper = false;
    for (uint32_t i = 0; i < items.Size(); ++i)
    {
        auto v = items.GetAt(i);
        if (v.ValueType() != JsonValueType::String)
            return false;

        hasWallpaper = true;
        if (!arrayContainsString(allowedItems, v.GetString().c_str()))
            return false;
    }
    return hasWallpaper;
}
static bool isActivePlaylistSuitableForTheme(JsonObject const& general, JsonArray const& playlists,
                                             ThemeTag desiredTheme, const std::wstring& lightAuto,
                                             const std::wstring& darkAuto, JsonArray const& lightItems,
                                             JsonArray const& darkItems)
{
    if (desiredTheme != ThemeTag::Light && desiredTheme != ThemeTag::Dark)
        return false;

    JsonObject activePlaylist;
    std::wstring activeName;
    if (!tryGetActivePlaylist(general, playlists, activePlaylist, activeName))
        return false;

    const std::wstring targetName = (desiredTheme == ThemeTag::Light) ? lightAuto : darkAuto;
    if (!activeName.empty() && activeName == targetName)
        return true;

    JsonArray const& targetItems = (desiredTheme == ThemeTag::Light) ? lightItems : darkItems;
    return playlistItemsFitTheme(activePlaylist, targetItems);
}
static bool setActivePlaylist(JsonObject& general, JsonArray const& playlists, const std::wstring& playlistName,
                              bool& changed)
{
    int idx = findPlaylistIndexByName(playlists, playlistName);
    if (idx < 0)
        return false;
    JsonObject srcPlaylist = playlists.GetAt(static_cast<uint32_t>(idx)).GetObject();
    JsonObject wc;
    if (!jsonTryGetObject(general, L"wallpaperconfig", wc))
    {
        wc = JsonObject{};
        general.SetNamedValue(L"wallpaperconfig", wrapObjectValue(wc));
    }
    JsonObject selected;
    if (!jsonTryGetObject(wc, L"selectedwallpapers", selected))
    {
        selected = JsonObject{};
        wc.SetNamedValue(L"selectedwallpapers", wrapObjectValue(selected));
    }
    JsonObject monitor0;
    if (!jsonTryGetObject(selected, L"Monitor0", monitor0))
    {
        monitor0 = JsonObject{};
        selected.SetNamedValue(L"Monitor0", wrapObjectValue(monitor0));
    }
    std::wstring currentName;
    bool same = false;
    if (monitor0.HasKey(L"playlist"))
    {
        auto pv = monitor0.GetNamedValue(L"playlist");
        if (pv.ValueType() == JsonValueType::Object && jsonTryGetString(pv.GetObject(), L"name", currentName) &&
            currentName == playlistName)
            same = true;
    }
    monitor0.SetNamedValue(L"playlist", wrapObjectValue(srcPlaylist));
    JsonArray items;
    if (jsonTryGetArray(srcPlaylist, L"items", items) && items.Size() > 0)
    {
        auto first = items.GetAt(0);
        if (first.ValueType() == JsonValueType::String)
            monitor0.SetNamedValue(L"file", JsonValue::CreateStringValue(first.GetString().c_str()));
    }
    selected.SetNamedValue(L"Monitor0", wrapObjectValue(monitor0));
    wc.SetNamedValue(L"selectedwallpapers", wrapObjectValue(selected));
    general.SetNamedValue(L"wallpaperconfig", wrapObjectValue(wc));
    if (!same)
        changed = true;
    return true;
}

static bool parseSchemecolor3(const std::wstring& s, double& r, double& g, double& b)
{
    r = g = b = 0.0;
    int n = swscanf_s(s.c_str(), L"%lf %lf %lf", &r, &g, &b);
    if (n != 3)
        return false;
    auto clamp01 = [](double& x)
    {
        if (x < 0.0)
            x = 0.0;
        if (x > 1.0)
            x = 1.0;
    };
    clamp01(r);
    clamp01(g);
    clamp01(b);
    return true;
}
static double srgbToLinear(double c01)
{
    return (c01 <= 0.04045) ? c01 / 12.92 : std::pow((c01 + 0.055) / 1.055, 2.4);
}
static double relativeLuminance(double r01, double g01, double b01)
{
    return 0.2126 * srgbToLinear(r01) + 0.7152 * srgbToLinear(g01) + 0.0722 * srgbToLinear(b01);
}
static double contrastRatio(double L1, double L2)
{
    double a = (std::max)(L1, L2);
    double b = (std::min)(L1, L2);
    return (a + 0.05) / (b + 0.05);
}
static double rgbToHueDeg(double r01, double g01, double b01)
{
    const double maxv = (std::max)(r01, (std::max)(g01, b01)), minv = (std::min)(r01, (std::min)(g01, b01)),
                 d = maxv - minv;
    if (d <= 1e-12)
        return 0.0;
    double h = 0.0;
    if (maxv == r01)
        h = (g01 - b01) / d + (g01 < b01 ? 6.0 : 0.0);
    else if (maxv == g01)
        h = (b01 - r01) / d + 2.0;
    else
        h = (r01 - g01) / d + 4.0;
    h *= 60.0;
    if (h < 0.0)
        h += 360.0;
    if (h >= 360.0)
        h -= 360.0;
    return h;
}
static ThemeTag classifyFromSchemecolor(const std::wstring& sc, double)
{
    double r, g, b;
    if (!parseSchemecolor3(sc, r, g, b))
        return ThemeTag::Unknown;
    const double L = relativeLuminance(r, g, b), hue = rgbToHueDeg(r, g, b);
    const bool isCool = (hue >= 180.0 && hue <= 270.0), isWarm = (hue >= 320.0 || hue <= 40.0);
    double thr = isCool ? 0.30 : (isWarm ? 0.18 : 0.28);
    const double cMax = (std::max)(r, (std::max)(g, b)), cMin = (std::min)(r, (std::min)(g, b));
    ThemeTag inferred = (L < thr) ? ThemeTag::Dark : ThemeTag::Light;
    if (isWarm && cMax >= 0.65 && (cMax > 1e-9 ? ((cMax - cMin) / cMax) : 0.0) >= 0.20)
        inferred = ThemeTag::Light;
    return inferred;
}

static bool jsonTryGetNumber(JsonObject const& obj, const std::wstring& key, double& out)
{
    if (!obj.HasKey(key))
        return false;
    auto v = obj.GetNamedValue(key);
    if (v.ValueType() != JsonValueType::Number)
        return false;
    out = v.GetNumber();
    return true;
}

static bool parseVec3String(const std::wstring& s, double& x, double& y, double& z)
{
    x = y = z = 0.0;
    return swscanf_s(s.c_str(), L"%lf %lf %lf", &x, &y, &z) >= 2;
}

static bool sceneValueToVec3(winrt::Windows::Data::Json::IJsonValue const& value, double& x, double& y, double& z)
{
    if (value.ValueType() == JsonValueType::String)
        return parseVec3String(value.GetString().c_str(), x, y, z);
    if (value.ValueType() == JsonValueType::Object)
    {
        std::wstring s;
        if (jsonTryGetString(value.GetObject(), L"value", s))
            return parseVec3String(s, x, y, z);
    }
    return false;
}

static bool sceneTryGetVec3(JsonObject const& obj, const std::wstring& key, double& x, double& y, double& z)
{
    if (!obj.HasKey(key))
        return false;
    return sceneValueToVec3(obj.GetNamedValue(key), x, y, z);
}

static double sceneReadScalar(JsonObject const& obj, const std::wstring& key, double fallback)
{
    if (!obj.HasKey(key))
        return fallback;
    auto v = obj.GetNamedValue(key);
    if (v.ValueType() == JsonValueType::Number)
        return v.GetNumber();
    if (v.ValueType() == JsonValueType::Object)
    {
        double n = fallback;
        if (jsonTryGetNumber(v.GetObject(), L"value", n))
            return n;
    }
    return fallback;
}

static bool sceneObjectVisible(JsonObject const& obj)
{
    if (!obj.HasKey(L"visible"))
        return true;
    auto v = obj.GetNamedValue(L"visible");
    if (v.ValueType() == JsonValueType::Boolean)
        return v.GetBoolean();
    if (v.ValueType() == JsonValueType::Object)
    {
        JsonObject o = v.GetObject();
        if (o.HasKey(L"value"))
        {
            auto vv = o.GetNamedValue(L"value");
            if (vv.ValueType() == JsonValueType::Boolean)
                return vv.GetBoolean();
        }
    }
    return true;
}

static std::string dirnameUtf8(const std::string& path)
{
    const size_t pos = path.find_last_of('/');
    return (pos == std::string::npos) ? std::string{} : path.substr(0, pos);
}

static std::string normalizePkgPathUtf8(std::string path)
{
    for (char& c : path)
        if (c == '\\')
            c = '/';
    while (!path.empty() && path.front() == '/')
        path.erase(path.begin());
    return path;
}

static bool parseVfsJson(const PkgParser& parser, const std::string& path, JsonObject& out)
{
    const auto& vfs = parser.GetVFS();
    auto it = vfs.find(path);
    if (it == vfs.end())
    {
        std::wstring targetW = sts::WStringFromUtf8(path);
        for (const auto& [vp, sp] : vfs)
        {
            if (sts::WStringFromUtf8(vp) == targetW)
            {
                std::string text(reinterpret_cast<const char*>(sp.data), sp.size);
                return JsonObject::TryParse(winrt::to_hstring(text), out);
            }
        }
        return false;
    }
    const MemSpan span = it->second;
    std::string text(reinterpret_cast<const char*>(span.data), span.size);
    return JsonObject::TryParse(winrt::to_hstring(text), out);
}

static bool resolveTexturePath(const PkgParser& parser, const std::string& materialDir, std::wstring textureName,
                               std::string& outPath)
{
    std::string tex = normalizePkgPathUtf8(utf16ToUtf8(textureName));
    if (tex.empty())
        return false;
    if (tex.size() < 4 || tex.substr(tex.size() - 4) != ".tex")
        tex += ".tex";

    const auto& vfs = parser.GetVFS();
    const std::string direct = tex;
    if (vfs.find(direct) != vfs.end())
    {
        outPath = direct;
        return true;
    }

    const std::string inMaterialDir = materialDir.empty() ? tex : (materialDir + "/" + tex);
    if (vfs.find(inMaterialDir) != vfs.end())
    {
        outPath = inMaterialDir;
        return true;
    }

    const std::string inMaterials = "materials/" + tex;
    if (vfs.find(inMaterials) != vfs.end())
    {
        outPath = inMaterials;
        return true;
    }

    const std::string suffix = "/" + tex;
    for (const auto& [path, span] : vfs)
    {
        (void)span;
        if (path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0)
        {
            outPath = path;
            return true;
        }
    }

    // Wide-string fallback for Unicode normalization mismatches
    std::wstring texW = textureName;
    if (texW.size() < 4 || texW.substr(texW.size() - 4) != L".tex")
        texW += L".tex";
    for (const auto& [vp, sp] : vfs)
    {
        (void)sp;
        std::wstring vW = sts::WStringFromUtf8(vp);
        if (vW.size() >= texW.size() && vW.compare(vW.size() - texW.size(), texW.size(), texW) == 0)
        {
            outPath = vp;
            return true;
        }
    }
    return false;
}

static bool resolveObjectTexturePath(const PkgParser& parser, JsonObject const& obj, std::string& outPath)
{
    std::wstring imagePathW;
    if (!jsonTryGetString(obj, L"image", imagePathW))
        return false;

    std::string modelPath = normalizePkgPathUtf8(utf16ToUtf8(imagePathW));
    if (modelPath.find("models/util/") == 0)
        return false;

    JsonObject model;
    if (!parseVfsJson(parser, modelPath, model))
        return false;

    std::wstring materialPathW;
    if (!jsonTryGetString(model, L"material", materialPathW))
        return false;

    std::string materialPath = normalizePkgPathUtf8(utf16ToUtf8(materialPathW));
    JsonObject material;
    if (!parseVfsJson(parser, materialPath, material))
        return false;

    JsonArray passes;
    if (!jsonTryGetArray(material, L"passes", passes) || passes.Size() == 0)
        return false;

    for (uint32_t i = 0; i < passes.Size(); ++i)
    {
        if (passes.GetAt(i).ValueType() != JsonValueType::Object)
            continue;
        JsonArray textures;
        if (!jsonTryGetArray(passes.GetAt(i).GetObject(), L"textures", textures))
            continue;
        for (uint32_t j = 0; j < textures.Size(); ++j)
        {
            auto texValue = textures.GetAt(j);
            if (texValue.ValueType() != JsonValueType::String)
                continue;
            if (resolveTexturePath(parser, dirnameUtf8(materialPath), texValue.GetString().c_str(), outPath))
                return true;
        }
    }
    return false;
}

static bool sceneAbsoluteOrigin(JsonObject const& obj, const std::unordered_map<int, JsonObject>& objectsById,
                                double& x, double& y, double& z, int depth = 0)
{
    x = y = z = 0.0;
    sceneTryGetVec3(obj, L"origin", x, y, z);
    if (depth > 8 || !obj.HasKey(L"parent") || obj.GetNamedValue(L"parent").ValueType() != JsonValueType::Number)
        return true;

    const int parentId = static_cast<int>(obj.GetNamedValue(L"parent").GetNumber());
    auto parentIt = objectsById.find(parentId);
    if (parentIt == objectsById.end())
        return true;

    double px = 0.0, py = 0.0, pz = 0.0;
    sceneAbsoluteOrigin(parentIt->second, objectsById, px, py, pz, depth + 1);
    x += px;
    y += py;
    z += pz;
    return true;
}

static bool calcSceneCompositeStatsFromPkg(const PkgParser& parser, double wPct, double hPct,
                                           const WallpaperAlignmentSettings& alignment, double& outRoiAvg,
                                           double& outRoiDark, double& outGlobalAvg, double& outGlobalDark,
                                           std::wstring& outDecodeSummary)
{
    JsonObject scene;
    if (!parseVfsJson(parser, "scene.json", scene))
        return false;

    JsonObject general, projection;
    double canvasW = 0.0, canvasH = 0.0;
    if (!jsonTryGetObject(scene, L"general", general) ||
        !jsonTryGetObject(general, L"orthogonalprojection", projection) ||
        !jsonTryGetNumber(projection, L"width", canvasW) || !jsonTryGetNumber(projection, L"height", canvasH) ||
        canvasW <= 0.0 || canvasH <= 0.0)
    {
        return false;
    }

    double clearR = 0.0, clearG = 0.0, clearB = 0.0;
    sceneTryGetVec3(general, L"clearcolor", clearR, clearG, clearB);

    JsonArray objects;
    if (!jsonTryGetArray(scene, L"objects", objects) || objects.Size() == 0)
        return false;

    std::unordered_map<int, JsonObject> objectsById;
    for (uint32_t i = 0; i < objects.Size(); ++i)
    {
        if (objects.GetAt(i).ValueType() != JsonValueType::Object)
            continue;
        JsonObject obj = objects.GetAt(i).GetObject();
        if (obj.HasKey(L"id") && obj.GetNamedValue(L"id").ValueType() == JsonValueType::Number)
            objectsById.emplace(static_cast<int>(obj.GetNamedValue(L"id").GetNumber()), obj);
    }

    struct Sample
    {
        double x = 0.0;
        double y = 0.0;
        bool insideCanvas = true;
        double r = 0.0;
        double g = 0.0;
        double b = 0.0;
    };

    const int w = static_cast<int>(canvasW);
    const int h = static_cast<int>(canvasH);
    const WallpaperPlacement placement = makeWallpaperPlacement(canvasW, canvasH, alignment);
    const int displayW = (std::max)(1, static_cast<int>(placement.displayW + 0.5));
    const int displayH = (std::max)(1, static_cast<int>(placement.displayH + 0.5));
    const int roiW = (std::max)(1, static_cast<int>(placement.displayW * wPct));
    const int roiH = (std::max)(1, static_cast<int>(placement.displayH * hPct));
    const int roiX = (std::max)(0, displayW - roiW);
    const int roiY = (std::max)(0, displayH - roiH);
    const int step = 4;

    std::vector<Sample> samples;
    samples.reserve(static_cast<size_t>((roiW / step + 2) * (roiH / step + 2)));
    for (int y = roiY; y < displayH; y += step)
    {
        for (int x = roiX; x < displayW; x += step)
        {
            double sx = 0.0, sy = 0.0;
            const bool inside = mapDisplayToSource(placement, x + 0.5, y + 0.5, sx, sy);
            samples.push_back(Sample{sx, sy, inside, clearR, clearG, clearB});
        }
    }
    if (samples.empty())
        return false;

    int decodedLayerCount = 0;
    std::vector<std::wstring> decodeFormats;

    auto rememberFormat = [&](const std::wstring& fmt)
    {
        if (fmt.empty())
            return;
        for (const auto& existing : decodeFormats)
        {
            if (existing == fmt)
                return;
        }
        decodeFormats.push_back(fmt);
    };

    const double* lut = GetSRGBLut();
    for (uint32_t i = 0; i < objects.Size(); ++i)
    {
        if (objects.GetAt(i).ValueType() != JsonValueType::Object)
            continue;
        JsonObject obj = objects.GetAt(i).GetObject();
        if (!sceneObjectVisible(obj))
            continue;

        double originX = 0.0, originY = 0.0, originZ = 0.0;
        sceneAbsoluteOrigin(obj, objectsById, originX, originY, originZ);

        double scaleX = 1.0, scaleY = 1.0, scaleZ = 1.0;
        sceneTryGetVec3(obj, L"scale", scaleX, scaleY, scaleZ);
        scaleX = std::abs(scaleX);
        scaleY = std::abs(scaleY);

        double sizeX = 0.0, sizeY = 0.0, sizeZ = 0.0;
        sceneTryGetVec3(obj, L"size", sizeX, sizeY, sizeZ);
        if (sizeX <= 0.0 || sizeY <= 0.0)
            continue;

        const double drawW = sizeX * scaleX;
        const double drawH = sizeY * scaleY;
        if (drawW <= 1.0 || drawH <= 1.0)
            continue;

        const double left = originX - drawW * 0.5;
        const double top = originY - drawH * 0.5;
        const double right = left + drawW;
        const double bottom = top + drawH;
        if (right < 0.0 || left > canvasW || bottom < 0.0 || top > canvasH)
            continue;

        const double objectAlpha = (std::max)(0.0, (std::min)(1.0, sceneReadScalar(obj, L"alpha", 1.0)));

        std::wstring imagePathW;
        jsonTryGetString(obj, L"image", imagePathW);
        if (imagePathW.find(L"models/util/solidlayer.json") != std::wstring::npos)
        {
            double sr = 1.0, sg = 1.0, sb = 1.0;
            sceneTryGetVec3(obj, L"color", sr, sg, sb);
            for (Sample& sample : samples)
            {
                if (!sample.insideCanvas)
                    continue;
                if (sample.x < left || sample.x >= right || sample.y < top || sample.y >= bottom)
                    continue;
                sample.r = sr * objectAlpha + sample.r * (1.0 - objectAlpha);
                sample.g = sg * objectAlpha + sample.g * (1.0 - objectAlpha);
                sample.b = sb * objectAlpha + sample.b * (1.0 - objectAlpha);
            }
            decodedLayerCount++;
            continue;
        }

        std::string texturePath;
        if (!resolveObjectTexturePath(parser, obj, texturePath))
            continue;

        auto texIt = parser.GetVFS().find(texturePath);
        if (texIt == parser.GetVFS().end())
            continue;

        PkgParser::RgbaImage img = parser.DecodeTexvToRGBA(texIt->second);
        if (!img.IsValid())
            continue;

        const int imageW = (img.imageWidth > 0) ? img.imageWidth : img.width;
        const int imageH = (img.imageHeight > 0) ? img.imageHeight : img.height;
        if (imageW <= 0 || imageH <= 0)
            continue;

        double tintR = 1.0, tintG = 1.0, tintB = 1.0;
        sceneTryGetVec3(obj, L"color", tintR, tintG, tintB);

        for (Sample& sample : samples)
        {
            if (!sample.insideCanvas)
                continue;
            if (sample.x < left || sample.x >= right || sample.y < top || sample.y >= bottom)
                continue;

            const double u = (static_cast<double>(sample.x) - left) / drawW;
            const double v = (static_cast<double>(sample.y) - top) / drawH;
            const int tx = (std::max)(0, (std::min)(imageW - 1, static_cast<int>(u * imageW)));
            const int ty = (std::max)(0, (std::min)(imageH - 1, static_cast<int>(v * imageH)));
            const uint8_t* px =
                img.pixels.data() + (static_cast<size_t>(ty) * static_cast<size_t>(img.width) + tx) * 4u;

            const double alpha = (px[3] / 255.0) * objectAlpha;
            if (alpha <= 0.001)
                continue;
            const double sr = (px[0] / 255.0) * tintR;
            const double sg = (px[1] / 255.0) * tintG;
            const double sb = (px[2] / 255.0) * tintB;
            sample.r = sr * alpha + sample.r * (1.0 - alpha);
            sample.g = sg * alpha + sample.g * (1.0 - alpha);
            sample.b = sb * alpha + sample.b * (1.0 - alpha);
        }

        decodedLayerCount++;
        rememberFormat(img.decodeFormat);
    }

    if (decodedLayerCount == 0)
        return false;

    double sumL = 0.0;
    int dark = 0;
    for (const Sample& sample : samples)
    {
        const int r = (std::max)(0, (std::min)(255, static_cast<int>(sample.r * 255.0 + 0.5)));
        const int g = (std::max)(0, (std::min)(255, static_cast<int>(sample.g * 255.0 + 0.5)));
        const int b = (std::max)(0, (std::min)(255, static_cast<int>(sample.b * 255.0 + 0.5)));
        const double L = 0.2126 * lut[r] + 0.7152 * lut[g] + 0.0722 * lut[b];
        sumL += L;
        if (L < 0.179)
            ++dark;
    }

    outRoiAvg = sumL / static_cast<double>(samples.size());
    outRoiDark = static_cast<double>(dark) / static_cast<double>(samples.size());
    outGlobalAvg = outRoiAvg;
    outGlobalDark = outRoiDark;

    outDecodeSummary.clear();
    for (size_t i = 0; i < decodeFormats.size(); ++i)
    {
        if (i != 0)
            outDecodeSummary += L"+";
        outDecodeSummary += decodeFormats[i];
    }
    if (outDecodeSummary.empty())
        outDecodeSummary = L"solid";
    return true;
}

struct DiagCounters
{
    int tagAlreadyPresent = 0;
    int missingConfigScheme = 0;
    int configSchemeParseFail = 0;
    int configSchemeLowDelta = 0;
    int missingProjectJson = 0;
    int projectJsonNoScheme = 0;
    int projectJsonParseFail = 0;
    int projectSchemeParseFail = 0;
    int projectSchemeLowDelta = 0;
    int finalUnknown = 0;
};
static void printDiag(const DiagCounters& d, const UpdateResult& r)
{
    std::wcout << L"\n[离线分类报告]\n  总壁纸: " << r.wallpapersTotal << L"  |  成功打标: " << r.wallpapersTagged
               << L"  |  无法识别: " << d.finalUnknown << std::endl;
}
static void scanProjectJsonUnderRoot(const std::wstring& rootDir, std::vector<std::wstring>& outProjectJsons)
{
    if (rootDir.empty() || !dirExists(rootDir))
        return;
    std::wstring pattern = joinPath(rootDir, L"*");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        std::wstring pj = joinPath(joinPath(rootDir, fd.cFileName), L"project.json");
        if (fileExists(pj))
            outProjectJsons.push_back(pj);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ===========================================================
// 【新增】多线程纯计算单元 (绝对不碰 JSON)
// ============================================================
struct EvalTaskResult
{
    std::wstring dictKey;      // wprops 里的原始键名
    std::wstring canonicalKey; // 标准化后的路径
    std::wstring wpId;         // 用于日志打印的壁纸 ID
    std::wstring projectJson;  // 降级用的 project.json 路径
    ThemeTag inferredTag = ThemeTag::Unknown;
    bool isNew = false;
    bool mfUsed = false;
    bool pkgParsed = false;
    bool pkgSceneComposite = false;
    bool alignmentApplied = false;
    std::wstring pkgDecodeFormat;
};

static EvalTaskResult EvaluateWallpaperHeavy(std::wstring dictKey, std::wstring canonicalKey, std::wstring wpId,
                                             std::wstring pjPath, bool isNew, ApplyOptions opt,
                                             WallpaperAlignmentSettings alignment)
{
    // 仅初始化 COM 单元，因为 MF 已经在 ApplyAndSwitch 中全局启动了！
    struct ComGuard
    {
        HRESULT hr;
        ComGuard()
        {
            hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        }
        ~ComGuard()
        {
            if (SUCCEEDED(hr))
                CoUninitialize();
        }
    } comGuard;

    EvalTaskResult res{dictKey, canonicalKey, wpId, pjPath, ThemeTag::Unknown, isNew, false};

    auto endsWith = [](const std::wstring& str, const std::wstring& suffix)
    { return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0; };
    std::wstring lowerKey = toLower(canonicalKey);

    // 1. MF 动态抽帧初筛
    if (endsWith(lowerKey, L".mp4") || endsWith(lowerKey, L".webm") || endsWith(lowerKey, L".avi") ||
        endsWith(lowerKey, L".mkv") || endsWith(lowerKey, L".mov"))
    {
        double rAvg = 0.0, rDark = 0.0, gAvg = 0.0, gDark = 0.0;
        if (calcVideoRoiStatsMF(canonicalKey, opt.trayRoiWidthPct, opt.trayRoiHeightPct, alignment, rAvg, rDark, gAvg,
                                gDark))
        {
            { ClassifyFeatures _f; _f.roiAvg=rAvg; _f.roiDarkRatio=rDark; _f.globalAvg=gAvg; _f.globalDarkRatio=gDark; auto _cr=ClassifyByStats(_f); res.inferredTag=_cr.tag; }
            res.mfUsed = true;
            res.alignmentApplied = alignment.custom;
            return res;
        }
    }

    // =========================================================
    // 3. 【新增装载】终极防线：极速内存硬解 scene.pkg
    // =========================================================
    if (res.inferredTag == ThemeTag::Unknown)
    {
        std::wstring actualDir = getWallpaperDir(canonicalKey);
        std::wstring pkgPath = joinPath(actualDir, L"scene.pkg");

        if (fileExists(pkgPath))
        {
            PkgParser parser;
            if (parser.Parse(pkgPath))
            {
                double rAvg = 0.0, rDark = 0.0, gAvg = 0.0, gDark = 0.0;
                std::wstring decodeSummary;
                if (calcSceneCompositeStatsFromPkg(parser, opt.trayRoiWidthPct, opt.trayRoiHeightPct, alignment, rAvg,
                                                   rDark, gAvg, gDark, decodeSummary))
                {
                    { ClassifyFeatures _f; _f.roiAvg=rAvg; _f.roiDarkRatio=rDark; _f.globalAvg=gAvg; _f.globalDarkRatio=gDark; auto _cr=ClassifyByStats(_f); res.inferredTag=_cr.tag; }
                    res.pkgParsed = true;
                    res.pkgSceneComposite = true;
                    res.alignmentApplied = alignment.custom;
                    res.pkgDecodeFormat = decodeSummary;
                }
                else
                {
                    std::string bgMedia = parser.FindBackgroundMedia();
                    if (!bgMedia.empty())
                    {
                        MemSpan span = parser.GetVFS().at(bgMedia);
                        PkgParser::RgbaImage img = parser.DecodeTexvToRGBA(span);
                        if (img.IsValid())
                        {
                            if ((!alignment.custom &&
                                 parser.CalcStatsFromRgba(img, opt.trayRoiWidthPct, opt.trayRoiHeightPct, rAvg, rDark,
                                                          gAvg, gDark)) ||
                                (alignment.custom &&
                                 calcRgbaRoiStatsAligned(img, opt.trayRoiWidthPct, opt.trayRoiHeightPct, alignment,
                                                         rAvg, rDark, gAvg, gDark)))
                            {
                                { ClassifyFeatures _f; _f.roiAvg=rAvg; _f.roiDarkRatio=rDark; _f.globalAvg=gAvg; _f.globalDarkRatio=gDark; auto _cr=ClassifyByStats(_f); res.inferredTag=_cr.tag; }
                                res.pkgParsed = true; // 亮起硬解成功指示灯
                                res.alignmentApplied = alignment.custom;
                                res.pkgDecodeFormat = img.decodeFormat;
                            }
                        }
                    }
                }
            }
        }
    }
    // =========================================================
    // 3. 终极降级：WIC 缩略图像素测算
    if (res.inferredTag == ThemeTag::Unknown && opt.enableTrayRoiL2)
    {
        std::wstring actualDir = getWallpaperDir(canonicalKey);
        res.inferredTag = evaluateL2TrayRoi(actualDir, opt, alignment);
        if (res.inferredTag != ThemeTag::Unknown)
            res.alignmentApplied = alignment.custom;
    }

    if (res.inferredTag == ThemeTag::Unknown && !pjPath.empty() && fileExists(pjPath))
    {
        std::wstring scheme;
        if (tryGetProjectJsonSchemecolor(pjPath, scheme))
            res.inferredTag = classifyFromSchemecolor(scheme, opt.minContrastDelta);
            if (res.inferredTag == ThemeTag::Both)
                res.inferredTag = ThemeTag::Light; // schemecolor Both -> Light
    }

    return res;
}

// ============================================================
// 【新增】用于延迟调度的受控并发任务结构体
// ============================================================
struct EvalTaskParams
{
    std::wstring rawKey;
    std::wstring canonicalKey;
    std::wstring wpId;
    std::wstring pjPath;
    bool isNew;
    WallpaperAlignmentSettings alignment;
};

// ============================================================
// Core Apply
// ============================================================
UpdateResult ApplyAndSwitch(const ApplyOptions& opt)
{
    UpdateResult r;
    if (opt.configPath.empty())
    {
        r.error = L"configPath is empty.";
        return r;
    }

    // =========================================================
    // 【终极修复 1】全局单次启动媒体引擎，彻底粉碎多线程反复初始化带来的底层残留
    // =========================================================
    struct GlobalMFGuard
    {
        bool ok;
        GlobalMFGuard()
        {
            ok = SUCCEEDED(MFStartup(MF_VERSION));
        }
        ~GlobalMFGuard()
        {
            if (ok)
                MFShutdown();
        }
    } globalMFGuard;
    // =========================================================

    DiagCounters diag{};
    winrt::init_apartment();
    std::string text;
    if (!readAllTextUtf8(opt.configPath, text))
    {
        r.error = L"Failed to read config.json";
        return r;
    }
    JsonObject root;
    try
    {
        if (!JsonObject::TryParse(winrt::to_hstring(text), root))
        {
            r.error = L"TryParse returned false.";
            return r;
        }
    }
    catch (...)
    {
        r.error = L"Exception parsing json.";
        return r;
    }
    r.loaded = true;

    if (root.HasKey(L"?installdirectory"))
    {
        auto v = root.GetNamedValue(L"?installdirectory");
        if (v.ValueType() == JsonValueType::String)
            r.weInstallDir = v.GetString().c_str();
    }
    r.profileKey = detectProfileKey(root);
    if (r.profileKey.empty())
    {
        r.error = L"Cannot detect profile key.";
        return r;
    }
    JsonObject profileObj = root.GetNamedValue(r.profileKey).GetObject();
    JsonObject general, wprops;
    if (!jsonTryGetObject(profileObj, L"general", general) || !jsonTryGetObject(profileObj, L"wproperties", wprops))
    {
        r.error = L"Profile missing general or wproperties.";
        return r;
    }
    std::wstring err;
    JsonArray playlists;
    if (!ensureAndGetPlaylists(general, playlists, err))
    {
        r.error = err;
        return r;
    }
    JsonObject settingsTemplate;
    if (playlists.Size() > 0 && playlists.GetAt(0).ValueType() == JsonValueType::Object)
        jsonTryGetObject(playlists.GetAt(0).GetObject(), L"settings", settingsTemplate);
    if (settingsTemplate.Size() == 0)
    {
        settingsTemplate.SetNamedValue(L"delay", JsonValue::CreateNumberValue(60));
        settingsTemplate.SetNamedValue(L"mode", JsonValue::CreateStringValue(L"logon"));
        settingsTemplate.SetNamedValue(L"order", JsonValue::CreateStringValue(L"random"));
        settingsTemplate.SetNamedValue(L"transition", JsonValue::CreateStringValue(L"0"));
        settingsTemplate.SetNamedValue(L"transitiontime", JsonValue::CreateNumberValue(1000));
        settingsTemplate.SetNamedValue(L"updateonpause", JsonValue::CreateBooleanValue(false));
        settingsTemplate.SetNamedValue(L"videosequence", JsonValue::CreateBooleanValue(false));
    }
    const std::wstring lightAuto = opt.lightAutoPlaylistName.empty() ? L"White_auto" : opt.lightAutoPlaylistName;
    const std::wstring darkAuto = opt.darkAutoPlaylistName.empty() ? L"Black_auto" : opt.darkAutoPlaylistName;
    int lightIdx = -1, darkIdx = -1;
    bool createdLight = ensurePlaylist(playlists, lightAuto, settingsTemplate, lightIdx);
    bool createdDark = ensurePlaylist(playlists, darkAuto, settingsTemplate, darkIdx);
    r.playlistsEnsured = true;
    if (createdLight || createdDark)
        r.changed = true;
    JsonObject lightObj = playlists.GetAt(static_cast<uint32_t>(lightIdx)).GetObject();
    JsonObject darkObj = playlists.GetAt(static_cast<uint32_t>(darkIdx)).GetObject();
    JsonArray lightItems, darkItems;
    if (!jsonTryGetArray(lightObj, L"items", lightItems))
    {
        JsonArray i;
        lightObj.SetNamedValue(L"items", wrapArrayValue(i));
        lightItems = i;
        r.changed = true;
    }
    if (!jsonTryGetArray(darkObj, L"items", darkItems))
    {
        JsonArray i;
        darkObj.SetNamedValue(L"items", wrapArrayValue(i));
        darkItems = i;
        r.changed = true;
    }
    bool playlistsMutated = false;

    std::unordered_set<std::wstring> currentLightSet;
    std::unordered_set<std::wstring> currentLightDirSet;
    for (uint32_t i = 0; i < lightItems.Size(); ++i)
    {
        if (lightItems.GetAt(i).ValueType() == JsonValueType::String)
        {
            std::wstring item = lightItems.GetAt(i).GetString().c_str();
            currentLightSet.insert(canonicalPathCompareKey(item));
            currentLightDirSet.insert(canonicalPathCompareKey(getWallpaperDir(item)));
        }
    }
    std::unordered_set<std::wstring> currentDarkSet;
    std::unordered_set<std::wstring> currentDarkDirSet;
    for (uint32_t i = 0; i < darkItems.Size(); ++i)
    {
        if (darkItems.GetAt(i).ValueType() == JsonValueType::String)
        {
            std::wstring item = darkItems.GetAt(i).GetString().c_str();
            currentDarkSet.insert(canonicalPathCompareKey(item));
            currentDarkDirSet.insert(canonicalPathCompareKey(getWallpaperDir(item)));
        }
    }

    auto applyTagAndPlaylist = [&](const std::wstring& wallpaperKey, JsonObject& monitor0, ThemeTag tag)
    {
        if (tag == ThemeTag::Unknown)
            return;
        ThemeTag existing = ThemeTag::Unknown;
        if (!tryReadThemeTag(monitor0, existing) || (opt.forceReclassifyExistingTags && existing != tag))
        {
            if (writeThemeTag(monitor0, tag))
            {
                if (existing == ThemeTag::Unknown)
                    r.wallpapersNewlyTagged++;
                r.changed = true;
            }
        }
        std::wstring key = canonicalizePathKey(wallpaperKey);
        if (tag == ThemeTag::Light)
        {
            bool removedLightAlias = removeWallpaperFromArray(lightItems, key, true);
            bool removed = removeWallpaperFromArray(darkItems, key, false);
            bool added = addUniqueString(lightItems, key);
            playlistsMutated = playlistsMutated || removedLightAlias || removed || added;
        }
        else if (tag == ThemeTag::Dark)
        {
            bool removedDarkAlias = removeWallpaperFromArray(darkItems, key, true);
            bool removed = removeWallpaperFromArray(lightItems, key, false);
            bool added = addUniqueString(darkItems, key);
            playlistsMutated = playlistsMutated || removedDarkAlias || removed || added;
        }
        else if (tag == ThemeTag::Both)
        {
            bool removedLightAlias = removeWallpaperFromArray(lightItems, key, true);
            bool removedDarkAlias = removeWallpaperFromArray(darkItems, key, true);
            bool addedLight = addUniqueString(lightItems, key);
            bool addedDark = addUniqueString(darkItems, key);
            playlistsMutated = playlistsMutated || removedLightAlias || removedDarkAlias || addedLight || addedDark;
        }
        else if (tag == ThemeTag::Ignore)
        {
            bool removedLight = removeWallpaperFromArray(lightItems, key, false);
            bool removedDark = removeWallpaperFromArray(darkItems, key, false);
            bool removedOther = removeStringFromOtherPlaylists(playlists, lightIdx, darkIdx, key);
            playlistsMutated = playlistsMutated || removedLight || removedDark || removedOther;
        }
    };

    // 改为收集任务参数，而不是直接发射线程
    std::vector<EvalTaskParams> pendingTasks;

    std::vector<std::wstring> wallpaperKeys;
    for (auto const& kv : wprops)
        wallpaperKeys.emplace_back(kv.Key().c_str());
    std::unordered_map<std::wstring, std::wstring> existingDirsByPathKey;
    existingDirsByPathKey.reserve(wallpaperKeys.size() * 2 + 16);

    for (auto const& rawKey : wallpaperKeys)
    {
        std::wstring dictKey = rawKey;
        std::wstring wallpaperKey = canonicalizePathKey(dictKey);
        std::wstring resolvedKey;
        std::wstring wpId = getWallpaperDir(wallpaperKey);
        auto slash = wpId.find_last_of(L'/');
        if (slash != std::wstring::npos)
            wpId = wpId.substr(slash + 1);

        if (!tryResolveExistingWallpaperFile(wallpaperKey, resolvedKey))
        {
            bool removedLight = removeWallpaperFromArray(lightItems, wallpaperKey, false);
            bool removedDark = removeWallpaperFromArray(darkItems, wallpaperKey, false);
            bool removedOther = removeStringFromOtherPlaylists(playlists, lightIdx, darkIdx, wallpaperKey);
            playlistsMutated = playlistsMutated || removedLight || removedDark || removedOther;
            wprops.Remove(rawKey);
            r.changed = true;

            if (opt.printDiagnostics)
                std::wcout << L"  [幽灵壁纸清理] 壁纸ID: " << wpId << L" -> 文件不存在，已从配置中移除" << std::endl;
            continue;
        }

        resolvedKey = canonicalizePathKey(resolvedKey);
        if (!samePathKey(wallpaperKey, resolvedKey))
        {
            auto sourceVal = wprops.GetNamedValue(rawKey);
            std::wstring exactResolvedKey;
            if (!findObjectPathKey(wprops, resolvedKey, exactResolvedKey))
            {
                wprops.SetNamedValue(resolvedKey, sourceVal);
                wprops.Remove(rawKey);
                dictKey = resolvedKey;
                r.changed = true;
            }
            else
            {
                dictKey = exactResolvedKey;
                if (!samePathKey(exactResolvedKey, rawKey))
                {
                    auto targetVal = wprops.GetNamedValue(exactResolvedKey);
                    if (sourceVal.ValueType() == JsonValueType::Object && targetVal.ValueType() == JsonValueType::Object)
                    {
                        JsonObject sourceEntry = sourceVal.GetObject();
                        JsonObject targetEntry = targetVal.GetObject();
                        JsonObject sourceMonitor0;
                        if (jsonTryGetObject(sourceEntry, L"Monitor0", sourceMonitor0))
                        {
                            JsonObject targetMonitor0;
                            ensureMonitor0Object(targetEntry, targetMonitor0);
                            bool merged = false;
                            for (auto const& kv : sourceMonitor0)
                            {
                                if (!targetMonitor0.HasKey(kv.Key()))
                                {
                                    targetMonitor0.SetNamedValue(kv.Key(), kv.Value());
                                    merged = true;
                                }
                            }
                            if (merged)
                            {
                                targetEntry.SetNamedValue(L"Monitor0", wrapObjectValue(targetMonitor0));
                                wprops.SetNamedValue(exactResolvedKey, wrapObjectValue(targetEntry));
                            }
                        }
                    }
                    wprops.Remove(rawKey);
                    r.changed = true;
                }
            }

            if (opt.printDiagnostics)
            {
                std::wstring resolvedWpId = getWallpaperDir(resolvedKey);
                auto resolvedSlash = resolvedWpId.find_last_of(L'/');
                if (resolvedSlash != std::wstring::npos)
                    resolvedWpId = resolvedWpId.substr(resolvedSlash + 1);
                std::wstring resolvedName = resolvedKey;
                auto nameSlash = resolvedName.find_last_of(L'/');
                if (nameSlash != std::wstring::npos)
                    resolvedName = resolvedName.substr(nameSlash + 1);
                std::wcout << L"  [真实文件修正] 壁纸ID: " << resolvedWpId << L" -> 使用实际文件 "
                           << resolvedName << std::endl;
            }

            wallpaperKey = resolvedKey;
            wpId = getWallpaperDir(wallpaperKey);
            auto resolvedSlash = wpId.find_last_of(L'/');
            if (resolvedSlash != std::wstring::npos)
                wpId = wpId.substr(resolvedSlash + 1);
        }

        std::wstring dirKey = canonicalPathCompareKey(getWallpaperDir(wallpaperKey));
        existingDirsByPathKey.emplace(dirKey, dictKey);

        auto entryVal = wprops.GetNamedValue(dictKey);
        if (entryVal.ValueType() != JsonValueType::Object)
            continue;
        JsonObject entryObj = entryVal.GetObject();
        JsonObject monitor0;
        ensureMonitor0Object(entryObj, monitor0);
        WallpaperAlignmentSettings alignment = readWallpaperAlignment(monitor0);
        r.wallpapersTotal++;

        ThemeTag tag = ThemeTag::Unknown;
        bool hasTag = tryReadThemeTag(monitor0, tag);

        // --- 【新增第二处】侦测用户手动修改：移动或踢出 ---
        const std::wstring wallpaperCompareKey = canonicalPathCompareKey(wallpaperKey);
        const std::wstring wallpaperDirCompareKey = canonicalPathCompareKey(getWallpaperDir(wallpaperKey));
        bool inLight = currentLightSet.count(wallpaperCompareKey) > 0 ||
                       currentLightDirSet.count(wallpaperDirCompareKey) > 0;
        bool inDark = currentDarkSet.count(wallpaperCompareKey) > 0 ||
                      currentDarkDirSet.count(wallpaperDirCompareKey) > 0;

        // --- 【新增1】读取是否已有免死金牌 ---
        bool userOverridden = false;
        if (monitor0.HasKey(L"sts_user_override"))
        {
            auto ov = monitor0.GetNamedValue(L"sts_user_override");
            if (ov.ValueType() == JsonValueType::Boolean && ov.GetBoolean())
            {
                userOverridden = true;
            }
        }
        // ------------------------------------

        ThemeTag userTag = ThemeTag::Unknown;

        if (hasTag && tag != ThemeTag::Ignore)
        {
            if (inLight && inDark)
            {
                userTag = ThemeTag::Both;
            }
            else if (inLight)
            {
                userTag = ThemeTag::Light;
            }
            else if (inDark)
            {
                userTag = ThemeTag::Dark;
            }
            else
            {
                userTag = ThemeTag::Ignore;
            }
        }

        if (userTag != ThemeTag::Unknown && userTag != tag)
        {
            tag = userTag;
            hasTag = true;
            userOverridden = true;
            writeThemeTag(monitor0, tag);
            // --- 【新增2】颁发免死金牌并永久存入 JSON ---
            monitor0.SetNamedValue(L"sts_user_override", JsonValue::CreateBooleanValue(true));
            // ---------------------------------------------
            r.changed = true;
            if (opt.printDiagnostics)
            {
                std::wcout << L"  [用户意图覆盖] 侦测到手动修改，壁纸ID: " << wpId << L" -> 强制同步为: "
                           << ThemeTagToString(tag) << L" (已颁发免死金牌)" << std::endl;
            }
        }

        if ((hasTag && tag == ThemeTag::Ignore) || ((hasTag && !opt.forceReclassifyExistingTags) || userOverridden))
        {
            diag.tagAlreadyPresent++;
            r.wallpapersTagged++;
            applyTagAndPlaylist(wallpaperKey, monitor0, tag);
        }
        else
        {
            // 【修改】将重度计算推入多线程后台，主线程直接 continue 放过它
            std::wstring dir = getWallpaperDir(wallpaperKey);
            std::wstring pj = dir.empty() ? L"" : (normalizeSlashes(dir) + L"/project.json");

            pendingTasks.push_back({dictKey, wallpaperKey, wpId, pj, false, alignment});

            continue; // 跳过当前循环后续的 JSON 写入，交给第三阶段处理
        }

        // 原有的 entryObj.SetNamedValue 保留，供不需要计算的壁纸使用
        entryObj.SetNamedValue(L"Monitor0", wrapObjectValue(monitor0));
        wprops.SetNamedValue(dictKey, wrapObjectValue(entryObj));
    }

    std::vector<std::wstring> projectJsons;
    if (!opt.workshopRoot431960.empty())
        scanProjectJsonUnderRoot(opt.workshopRoot431960, projectJsons);
    if (!opt.myProjectsRoot.empty())
        scanProjectJsonUnderRoot(opt.myProjectsRoot, projectJsons);

    for (auto const& pj : projectJsons)
    {
        r.discoveredTotal++;
        std::wstring mainKey;
        if (!tryGetMainWallpaperKeyFromProjectJson(pj, mainKey))
            continue;
        mainKey = canonicalizePathKey(mainKey);
        std::wstring resolvedMainKey;
        if (!tryResolveExistingWallpaperFile(mainKey, resolvedMainKey))
        {
            if (opt.printDiagnostics)
            {
                std::wstring wpId = getParentDir(pj);
                auto slash = wpId.find_last_of(L'/');
                if (slash != std::wstring::npos)
                    wpId = wpId.substr(slash + 1);
                std::wcout << L"  [发现隐藏壁纸] 壁纸ID: " << wpId << L" -> 主文件不存在，跳过测算" << std::endl;
            }
            continue;
        }
        mainKey = canonicalizePathKey(resolvedMainKey);
        std::wstring dirKey = canonicalPathCompareKey(getWallpaperDir(mainKey));
        if (existingDirsByPathKey.find(dirKey) != existingDirsByPathKey.end())
            continue;

        std::wstring wpId = getParentDir(pj);
        auto slash = wpId.find_last_of(L'/');
        if (slash != std::wstring::npos)
            wpId = wpId.substr(slash + 1);

        pendingTasks.push_back({mainKey, mainKey, wpId, pj, true, WallpaperAlignmentSettings{}});
        existingDirsByPathKey.emplace(dirKey, mainKey);
    }

    // ==========================================================
    // 【修改】第三阶段：受控并发执行 (彻底解决内存爆炸问题)
    // ==========================================================
    if (!pendingTasks.empty() && opt.printDiagnostics)
    {
        std::wcout << L"  [并发调度] 共收集 " << pendingTasks.size() << L" 个重度测算任务，开始受控分批执行..."
                   << std::endl;
    }

    // 获取 CPU 物理核心数，强制设定安全上限（避免爆内存，8 个并发足以吃满多数 CPU）
    size_t maxThreads = std::thread::hardware_concurrency();
    if (maxThreads == 0)
        maxThreads = 4;
    maxThreads = 4; // 当前策略固定串行，避免线程创建与缓存回收开销

    // 分批次 (Chunk) 发射任务
    for (size_t i = 0; i < pendingTasks.size(); i += maxThreads)
    {
        size_t batchEnd = (std::min<size_t>)(i + maxThreads, pendingTasks.size());

        std::vector<EvalTaskResult> batchResults(batchEnd - i);

        for (size_t j = i; j < batchEnd; ++j)
        {
            const size_t localIdx = j - i;
            const auto& task = pendingTasks[j];
            batchResults[localIdx] =
                EvaluateWallpaperHeavy(task.rawKey, task.canonicalKey, task.wpId, task.pjPath, task.isNew, opt,
                                       task.alignment);
        }
        // =========================================================
        // 【终极修复 2】击碎 Windows 系统的“文件缓存假象”！
        // 强制操作系统立刻没收当前进程占用的所有映射文件和视频物理缓冲
        // =========================================================
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

        // 3. 线程死绝后，内存已清空，主线程安全地将结果写入 JSON
        for (auto& res : batchResults)
        {
            if (res.inferredTag == ThemeTag::Unknown)
            {
                diag.finalUnknown++;
                if (opt.printDiagnostics)
                    std::wcout << L"  [并发测算] 壁纸ID: " << res.wpId << L" -> [无法识别/丢弃]" << std::endl;
                continue;
            }

            r.wallpapersTotal++;
            r.wallpapersTagged++;

            JsonObject entryObj;
            if (!res.isNew && wprops.HasKey(res.dictKey))
            {
                entryObj = wprops.GetNamedValue(res.dictKey).GetObject();
            }
            else if (res.isNew)
            {
                r.discoveredInserted++;
                r.wallpapersNewlyTagged++;
            }

            JsonObject monitor0;
            ensureMonitor0Object(entryObj, monitor0);

            if (writeThemeTag(monitor0, res.inferredTag))
            {
                r.changed = true;
                if (!res.isNew)
                    r.wallpapersNewlyTagged++;
            }

            entryObj.SetNamedValue(L"Monitor0", wrapObjectValue(monitor0));
            wprops.SetNamedValue(res.dictKey, wrapObjectValue(entryObj));

            applyTagAndPlaylist(res.canonicalKey, monitor0, res.inferredTag);

            if (opt.printDiagnostics)
            {
                std::wstring methodLog;
                if (res.mfUsed)
                    methodLog = L" (MF动态抽帧)";
                else if (res.pkgSceneComposite)
                    methodLog = L" (PKG合成硬解:" + res.pkgDecodeFormat + L")";
                else if (res.pkgParsed)
                    methodLog = L" (PKG硬解:" + res.pkgDecodeFormat + L")";
                else
                    methodLog = L" (静态降级)";

                if (res.alignmentApplied)
                    methodLog += L" + 对齐配置";

                std::wcout << L"  [并发测算] 壁纸ID: " << res.wpId << (res.isNew ? L" (新发现)" : L"") << methodLog
                           << L" -> 判定为: " << ThemeTagToString(res.inferredTag) << std::endl;
            }
        }
    }
    // ==========================================================

    if (playlistsMutated)
    {
        r.playlistsUpdated = true;
        r.changed = true;
        lightObj.SetNamedValue(L"items", wrapArrayValue(lightItems));
        darkObj.SetNamedValue(L"items", wrapArrayValue(darkItems));
        playlists.SetAt(static_cast<uint32_t>(lightIdx), wrapObjectValue(lightObj));
        playlists.SetAt(static_cast<uint32_t>(darkIdx), wrapObjectValue(darkObj));
    }
    if (opt.desiredTheme == ThemeTag::Light || opt.desiredTheme == ThemeTag::Dark)
    {
        std::wstring target = (opt.desiredTheme == ThemeTag::Light) ? lightAuto : darkAuto;
        const bool activeSuitable =
            isActivePlaylistSuitableForTheme(general, playlists, opt.desiredTheme, lightAuto, darkAuto, lightItems,
                                             darkItems);
        r.activePlaylistAlreadySuitable = activeSuitable;

        if (opt.preserveActivePlaylistWhenSuitable && activeSuitable)
        {
            r.activePlaylistPreserved = true;
            if (opt.printDiagnostics)
            {
                std::wcout << L"  [启动保护] 当前播放列表已满足 "
                           << ThemeTagToString(opt.desiredTheme) << L" 主题，跳过播放列表切换。" << std::endl;
            }
        }
        else
        {
            bool localChanged = false;
            if (setActivePlaylist(general, playlists, target, localChanged))
            {
                if (localChanged)
                {
                    r.switchedPlaylist = true;
                    r.changed = true;
                }
            }
        }
    }
    general.SetNamedValue(L"playlists", wrapArrayValue(playlists));
    profileObj.SetNamedValue(L"general", wrapObjectValue(general));
    profileObj.SetNamedValue(L"wproperties", wrapObjectValue(wprops));
    root.SetNamedValue(r.profileKey, wrapObjectValue(profileObj));
    if (opt.printDiagnostics)
        printDiag(diag, r);
    if (!r.changed)
        return r;

    if (opt.manageWallpaperEngineProcess)
    {
        if (isWallpaperEngineRunning())
        {
            r.attemptedGracefulExit = true;
            if (!tryGracefulExitWallpaperEngine(opt.gracefulExitTimeoutMs))
            {
                r.performedHardKill = true;
                killWallpaperEngineHard();
                waitProcessExitByName(3000);
            }
        }
        if (!waitFileWritable(opt.configPath, opt.fileWritableTimeoutMs))
        {
            r.error = L"config.json is not writable (still locked).";
            return r;
        }
    }
    std::string outText = utf16ToUtf8(prettyJson(root.Stringify().c_str(), 2));
    std::wstring saveErr;

    backupCleanConfig(opt, root);

    if (!atomicReplaceFile(opt.configPath, outText, saveErr))
    {
        r.error = saveErr;
        return r;
    }
    r.saved = true;
    if (opt.manageWallpaperEngineProcess)
    {
        std::wstring exePath = opt.wallpaperEngineExeOverride;
        if (exePath.empty())
            exePath = inferWallpaperEngineExe(r.weInstallDir);
        if (!exePath.empty())
        {
            std::wstring startErr;
            if (startWallpaperEngineExe(exePath, startErr))
                r.restartedWe = true;
            else
                r.error = startErr;
        }
    }
    return r;
}
} // namespace sts::we
