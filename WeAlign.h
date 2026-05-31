#pragma once
#include <windows.h>
#include <winrt/Windows.Data.Json.h>
using namespace winrt::Windows::Data::Json;

namespace sts::we
{

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

WallpaperAlignmentSettings ReadWallpaperAlignment(JsonObject const& monitor0);
WallpaperPlacement MakeWallpaperPlacement(double sourceW, double sourceH, const WallpaperAlignmentSettings& align,
                                         double overrideDisplayW = 0.0, double overrideDisplayH = 0.0);
bool MapDisplayToSource(const WallpaperPlacement& p, double displayX, double displayY, double& outX, double& outY);

} // namespace sts::we
