#pragma once
#include "WeConfigManager.h"
#include "WeAlign.h"

namespace sts::we
{

struct ResolvedTexInfo
{
    std::string path;
    double cropX = 0.0;
    double cropY = 0.0;
    bool hasCrop = false;
};

bool CalcSceneCompositeStatsFromPkg(const PkgParser& parser, double wPct, double hPct,
                                    const WallpaperAlignmentSettings& alignment, double& outRoiAvg,
                                    double& outRoiDark, double& outGlobalAvg, double& outGlobalDark,
                                    std::wstring& outDecodeSummary);

bool RenderSceneCompositeToPng(const std::wstring& pkgPath, const std::wstring& outPngPath,
                               double& outAvgLuminance, double& outDarkRatio,
                               double& outGlobalAvg, double& outGlobalDark,
                               std::wstring& outDecodeSummary,
                               const WallpaperAlignmentSettings& alignment = WallpaperAlignmentSettings{});

bool TestCalcSceneComposite(const std::wstring& pkgPath,
                            double wPct, double hPct,
                            double& outRoiAvg, double& outRoiDark,
                            double& outGlobalAvg, double& outGlobalDark,
                            std::wstring& outDecodeSummary);

bool RenderBackgroundMediaToPng(const std::wstring& pkgPath, const std::wstring& outPngPath,
                                double wPct, double hPct,
                                double& outRoiAvg, double& outRoiDark,
                                double& outGlobalAvg, double& outGlobalDark,
                                std::wstring& outDecodeSummary);

} // namespace sts::we
