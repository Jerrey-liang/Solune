#include "WeConfigManager.h"
#include "WeAlign.h"
#include "WeClassify.h"
#include "WeScene.h"
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
#include <future>


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

// Forward declarations

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
bool jsonTryGetNumber(JsonObject const& obj, const std::wstring& key, double& out);

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
std::wstring normalizeSlashes(std::wstring s)
{
    for (auto& c : s)
        if (c == L'\\')
            c = L'/';
    return s;
}
std::wstring canonicalizePathKey(std::wstring p)
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
bool fileExists(const std::wstring& p)
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
std::wstring joinPath(const std::wstring& a, const std::wstring& b)
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
std::wstring getWallpaperDir(const std::wstring& path)
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
std::string utf16ToUtf8(const std::wstring& ws)
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
bool jsonTryGetObject(JsonObject const& obj, const std::wstring& key, JsonObject& out)
{
    if (!obj.HasKey(key))
        return false;
    auto v = obj.GetNamedValue(key);
    if (v.ValueType() != JsonValueType::Object)
        return false;
    out = v.GetObject();
    return true;
}
bool jsonTryGetArray(JsonObject const& obj, const std::wstring& key, JsonArray& out)
{
    if (!obj.HasKey(key))
        return false;
    auto v = obj.GetNamedValue(key);
    if (v.ValueType() != JsonValueType::Array)
        return false;
    out = v.GetArray();
    return true;
}
bool jsonTryGetString(JsonObject const& obj, const std::wstring& key, std::wstring& out)
{
    if (!obj.HasKey(key))
        return false;
    auto v = obj.GetNamedValue(key);
    if (v.ValueType() != JsonValueType::String)
        return false;
    out = v.GetString().c_str();
    return true;
}



bool jsonTryGetBool(JsonObject const& obj, const std::wstring& key, bool& out)
{
    if (!obj.HasKey(key))
        return false;
    auto v = obj.GetNamedValue(key);
    if (v.ValueType() != JsonValueType::Boolean)
        return false;
    out = v.GetBoolean();
    return true;
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


// Snapshot config.json without our classification tags
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

            // Strip wallpaper properties we added
            JsonObject wprops;
            if (jsonTryGetObject(profileObj, L"wproperties", wprops))
            {
                // Collect keys before modifying the collection
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

            // Remove playlists we created
            JsonObject general;
            if (jsonTryGetObject(profileObj, L"general", general))
            {
                std::wstring lightAuto = opt.lightAutoPlaylistName.empty() ? L"white_auto" : opt.lightAutoPlaylistName;
                std::wstring darkAuto = opt.darkAutoPlaylistName.empty() ? L"black_auto" : opt.darkAutoPlaylistName;

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
        MakeWallpaperPlacement(static_cast<double>(width), static_cast<double>(height), alignment);
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
            if (MapDisplayToSource(placement, dx + 0.5, dy + 0.5, sx, sy))
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
    pBuffer->Unlock();
    pBuffer = nullptr;
    pSample = nullptr;
    pReader = nullptr;
    return true;
}

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


bool jsonTryGetNumber(JsonObject const& obj, const std::wstring& key, double& out)
{
    if (!obj.HasKey(key))
        return false;
    auto v = obj.GetNamedValue(key);
    if (v.ValueType() != JsonValueType::Number)
        return false;
    out = v.GetNumber();
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

    // Scene PKG analysis
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
                if (CalcSceneCompositeStatsFromPkg(parser, opt.trayRoiWidthPct, opt.trayRoiHeightPct, alignment, rAvg,
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
                            double fillLuminance = 0.0;
                            if (alignment.custom)
                            {
                                std::wstring scheme;
                                if (tryGetProjectJsonSchemecolor(pjPath, scheme))
                                    fillLuminance = SchemecolorToLuminance(scheme);
                            }
                            if ((!alignment.custom &&
                                 parser.CalcStatsFromRgba(img, opt.trayRoiWidthPct, opt.trayRoiHeightPct, rAvg, rDark,
                                                          gAvg, gDark)) ||
                                (alignment.custom &&
                                 CalcRgbaRoiStatsAligned(img, opt.trayRoiWidthPct, opt.trayRoiHeightPct, alignment,
                                                         rAvg, rDark, gAvg, gDark, fillLuminance)))
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
    // Fallback: schemecolor classification
    if (res.inferredTag == ThemeTag::Unknown && !pjPath.empty() && fileExists(pjPath))
    {
        std::wstring scheme;
        if (tryGetProjectJsonSchemecolor(pjPath, scheme))
            res.inferredTag = ClassifyFromSchemecolor(scheme, opt.minContrastDelta);
            if (res.inferredTag == ThemeTag::Both)
                res.inferredTag = ThemeTag::Light; // schemecolor Both -> Light
    }

    return res;
}

struct EvalTaskParams
{
    std::wstring rawKey;
    std::wstring canonicalKey;
    std::wstring wpId;
    std::wstring pjPath;
    bool isNew;
    WallpaperAlignmentSettings alignment;
};

// ApplyAndSwitch
UpdateResult ApplyAndSwitch(const ApplyOptions& opt)
{
    UpdateResult r;
    if (opt.configPath.empty())
    {
        r.error = L"configPath is empty.";
        return r;
    }

    // Single MFStartup to avoid per-thread re-init issues
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
    const std::wstring lightAuto = opt.lightAutoPlaylistName.empty() ? L"white_auto" : opt.lightAutoPlaylistName;
    const std::wstring darkAuto = opt.darkAutoPlaylistName.empty() ? L"black_auto" : opt.darkAutoPlaylistName;
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
            bool removedDarkAlias = removeWallpaperFromArray(darkItems, key, true);
            bool removed = removeWallpaperFromArray(lightItems, key, false);
            bool added = addUniqueString(lightItems, key);
            playlistsMutated = playlistsMutated || removedDarkAlias || removed || added;
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
        WallpaperAlignmentSettings alignment = ReadWallpaperAlignment(monitor0);
        r.wallpapersTotal++;

        ThemeTag tag = ThemeTag::Unknown;
        bool hasTag = tryReadThemeTag(monitor0, tag);

        // Detect manual user changes (move or remove from playlist)
        const std::wstring wallpaperCompareKey = canonicalPathCompareKey(wallpaperKey);
        const std::wstring wallpaperDirCompareKey = canonicalPathCompareKey(getWallpaperDir(wallpaperKey));
        bool inLight = currentLightSet.count(wallpaperCompareKey) > 0 ||
                       currentLightDirSet.count(wallpaperDirCompareKey) > 0;
        bool inDark = currentDarkSet.count(wallpaperCompareKey) > 0 ||
                      currentDarkDirSet.count(wallpaperDirCompareKey) > 0;

        // Check for user-override flag
        bool userOverridden = false;
        if (monitor0.HasKey(L"sts_user_override"))
        {
            auto ov = monitor0.GetNamedValue(L"sts_user_override");
            if (ov.ValueType() == JsonValueType::Boolean && ov.GetBoolean())
            {
                userOverridden = true;
            }
        }
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
            // Write user-override flag to JSON
            monitor0.SetNamedValue(L"sts_user_override", JsonValue::CreateBooleanValue(true));
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
            // Defer heavy evaluation to background threads
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

    // Phase 3: concurrent batch evaluation
    if (!pendingTasks.empty() && opt.printDiagnostics)
    {
        std::wcout << L"  [并发调度] 共收集 " << pendingTasks.size() << L" 个重度测算任务，开始受控分批执行..."
                   << std::endl;
    }

    // 获取 CPU 物理核心数并发执行，设上限避免内存压力
    size_t maxThreads = std::thread::hardware_concurrency();
    if (maxThreads == 0)
        maxThreads = 4;
    if (maxThreads > 6)
        maxThreads = 6;

    // 分批次 (Chunk) 并发发射任务
    for (size_t i = 0; i < pendingTasks.size(); i += maxThreads)
    {
        size_t batchEnd = (std::min<size_t>)(i + maxThreads, pendingTasks.size());

        std::vector<std::future<EvalTaskResult>> futures;
        futures.reserve(batchEnd - i);

        for (size_t j = i; j < batchEnd; ++j)
        {
            futures.push_back(std::async(std::launch::async,
                [rawKey     = pendingTasks[j].rawKey,
                 canonicalKey = pendingTasks[j].canonicalKey,
                 wpId       = pendingTasks[j].wpId,
                 pjPath     = pendingTasks[j].pjPath,
                 isNew      = pendingTasks[j].isNew,
                 alignment  = pendingTasks[j].alignment,
                 opt]() {
                    return EvaluateWallpaperHeavy(rawKey, canonicalKey, wpId, pjPath, isNew, opt, alignment);
                }));
        }

        // 收集本批次所有结果
        std::vector<EvalTaskResult> batchResults;
        batchResults.reserve(futures.size());
        for (auto& f : futures)
            batchResults.push_back(f.get());

        // 强制操作系统立刻没收当前进程占用的所有映射文件和视频物理缓冲
        SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);

        // 主线程安全地将结果写入 JSON (串行，无竞态)
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

