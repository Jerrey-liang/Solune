#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace sts::we
{
    enum class ThemeTag
    {
        Light,
        Dark,
        Both,
        Ignore,
        Unknown
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

        bool enableTrayRoiL2 = true;
        double trayRoiWidthPct = 0.25;
        double trayRoiHeightPct = 0.08;
        double l2DarkThreshold = 0.28;

        bool forceReclassifyExistingTags = false;
        bool preserveActivePlaylistWhenSuitable = true;
        bool printDiagnostics = true;

        std::wstring workshopRoot431960;
        std::wstring myProjectsRoot;
    };

    UpdateResult ApplyAndSwitch(const ApplyOptions& opt);

    const wchar_t* ThemeTagToString(ThemeTag t);
    ThemeTag ThemeTagFromString(const std::wstring& s);
}

struct MemSpan
{
    const uint8_t* data = nullptr;
    size_t size = 0;
};

#pragma pack(push, 1)
struct TexvHeader
{
    char magic1[4];
    char version1[4];
    char null1;
    char magic2[4];
    char version2[4];
    char null2;
    int32_t format;
    int32_t flags;
    int32_t textureWidth;
    int32_t textureHeight;
    int32_t imageWidth;
    int32_t imageHeight;
    int32_t unkInt0;
};

struct TexbHeader
{
    char magic[4];
    char version[4];
    char null1;
};
#pragma pack(pop)

class PkgParser
{
public:
    struct RgbaImage
    {
        int width = 0;
        int height = 0;
        int imageWidth = 0;
        int imageHeight = 0;
        std::vector<uint8_t> pixels;
        int sourceFormat = -1;
        std::wstring decodeFormat;

        bool IsValid() const
        {
            return !pixels.empty();
        }
    };

    struct ThemeColor
    {
        int r = 0;
        int g = 0;
        int b = 0;
        bool isValid = false;
    };

    PkgParser();
    ~PkgParser();

    PkgParser(const PkgParser&) = delete;
    PkgParser& operator=(const PkgParser&) = delete;

    bool Parse(const std::wstring& filePath);
    const std::unordered_map<std::string, MemSpan>& GetVFS() const
    {
        return m_vfs;
    }

    std::string FindBackgroundMedia() const;
    MemSpan ExtractRawMediaFromTex(const MemSpan& texSpan) const;
    RgbaImage DecodeTexvToRGBA(const MemSpan& texSpan) const;
    ThemeColor CalculateThemeColor(const RgbaImage& image) const;
    bool CalcStatsFromRgba(const RgbaImage& img, double wPct, double hPct, double& outRoiAvg, double& outRoiDark,
                           double& outGlobalAvg, double& outGlobalDark) const;

private:
    void Cleanup();
    bool FailAndCleanup(const std::wstring& errorMsg);
    float CalculateBaseScore(const std::string& path, size_t fileSize) const;
    bool IsReferencedByJsons(const std::string& targetBaseName) const;

private:
    HANDLE m_hFile = INVALID_HANDLE_VALUE;
    HANDLE m_hMapping = nullptr;
    const uint8_t* m_basePtr = nullptr;
    size_t m_fileSize = 0;
    std::unordered_map<std::string, MemSpan> m_vfs;
};
