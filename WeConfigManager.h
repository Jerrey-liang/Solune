#pragma once

#include <string>
#include <windows.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include "PkgParser.h"

using namespace winrt::Windows::Data::Json;

namespace sts::we
{
    enum class ThemeTag
    {
        Light,
        Dark,
        Ignore,
        Unknown
    };

    struct ClassifyFeatures
    {
        double roiAvg = 0.0;
        double roiDarkRatio = 0.0;
        double globalAvg = 0.0;
        double globalDarkRatio = 0.0;

        double surroundAvg = -1.0;
        double edgeDensity = -1.0;
        double themeHue = -1.0;
        double themeSaturation = -1.0;
    };

    struct ClassifyResult
    {
        ThemeTag tag = ThemeTag::Unknown;
        double confidence = 0.0;
    };

    struct UpdateResult
    {
        bool loaded = false;
        bool changed = false;
        bool saved = false;

        bool playlistsEnsured = false;
        bool playlistsUpdated = false;
        bool switchedPlaylist = false;
        bool activePlaylistAlreadySuitable = false;
        bool activePlaylistPreserved = false;

        bool attemptedGracefulExit = false;
        bool performedHardKill = false;
        bool restartedWe = false;

        int wallpapersTotal = 0;
        int wallpapersTagged = 0;
        int wallpapersNewlyTagged = 0;

        int discoveredTotal = 0;
        int discoveredInserted = 0;

        int l2Evaluated = 0;
        int l2Applied = 0;
        int l2Skipped = 0;
        int l2Unknown = 0;

        std::wstring profileKey;
        std::wstring weInstallDir;
        std::wstring error;
    };

    struct ApplyOptions
    {
        std::wstring configPath;
        std::wstring lightAutoPlaylistName;
        std::wstring darkAutoPlaylistName;

        ThemeTag desiredTheme = ThemeTag::Unknown;

        bool manageWallpaperEngineProcess = true;
        unsigned gracefulExitTimeoutMs = 3000;
        unsigned fileWritableTimeoutMs = 4000;

        std::wstring wallpaperEngineExeOverride;

        double minContrastDelta = 0.20;

        double trayRoiWidthPct = 0.25;
        double trayRoiHeightPct = 0.08;

        double confidenceThreshold = 0.55;

        bool forceReclassifyExistingTags = false;
        bool preserveActivePlaylistWhenSuitable = true;
        bool printDiagnostics = true;

        std::wstring workshopRoot431960;
        std::wstring myProjectsRoot;
    };

    UpdateResult ApplyAndSwitch(const ApplyOptions& opt);

    std::string utf16ToUtf8(const std::wstring& ws);
    std::wstring normalizeSlashes(std::wstring s);
    bool        fileExists(const std::wstring& p);
    std::wstring joinPath(const std::wstring& a, const std::wstring& b);
    std::wstring getWallpaperDir(const std::wstring& path);
    std::wstring canonicalizePathKey(std::wstring p);
    bool        jsonTryGetNumber(JsonObject const& obj, const std::wstring& key, double& out);
    bool        jsonTryGetString(JsonObject const& obj, const std::wstring& key, std::wstring& out);
    bool        jsonTryGetArray (JsonObject const& obj, const std::wstring& key, JsonArray& out);
    bool        jsonTryGetObject(JsonObject const& obj, const std::wstring& key, JsonObject& out);
    bool        jsonTryGetBool  (JsonObject const& obj, const std::wstring& key, bool& out);

    const wchar_t* ThemeTagToString(ThemeTag t);
    ThemeTag ThemeTagFromString(const std::wstring& s);

    bool TryReadProjectJsonSchemecolor(const std::wstring& projectJsonPath, std::wstring& outScheme);
}
