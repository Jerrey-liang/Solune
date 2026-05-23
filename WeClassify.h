#pragma once
#include "WeConfigManager.h"
#include "WeAlign.h"

namespace sts::we
{

ClassifyResult ClassifyByStats(const ClassifyFeatures& f);
ThemeTag ClassifyFromSchemecolor(const std::wstring& sc, double minContrastDelta);

double rgbToLinearLuminance(uint8_t r, uint8_t g, uint8_t b);
const double* GetSRGBLut();

bool CalcRgbaRoiStatsAligned(const PkgParser::RgbaImage& img, double wPct, double hPct,
                             const WallpaperAlignmentSettings& alignment, double& outRoiAvg,
                             double& outRoiDark, double& outGlobalAvg, double& outGlobalDark);

} // namespace sts::we
