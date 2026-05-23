#include "WeClassify.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace sts::we
{

const double* GetSRGBLut()
{
    static const std::array<double, 256> lut = []()
    {
        std::array<double, 256> arr{};
        for (int i = 0; i < 256; ++i)
        {
            double c = i / 255.0;
            arr[i] = (c <= 0.04045) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
        }
        return arr;
    }();
    return lut.data();
}

ClassifyResult ClassifyByStats(const ClassifyFeatures& f)
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
    { tag = ThemeTag::Dark; conf = 0.78; }
    else if (f.globalAvg >= 0.32 && f.roiAvg >= 0.30 && f.globalDarkRatio <= 0.38 && f.roiDarkRatio <= 0.30)
    { tag = ThemeTag::Light; conf = 0.75; }
    else
    { tag = ThemeTag::Dark; conf = 0.60; }

    if (conf > 1.0) conf = 1.0;
    return {tag, conf};
}

double rgbToLinearLuminance(uint8_t r, uint8_t g, uint8_t b)
{
    const double* lut = GetSRGBLut();
    return 0.2126 * lut[r] + 0.7152 * lut[g] + 0.0722 * lut[b];
}

bool CalcRgbaRoiStatsAligned(const PkgParser::RgbaImage& img, double wPct, double hPct,
                             const WallpaperAlignmentSettings& alignment, double& outRoiAvg,
                             double& outRoiDark, double& outGlobalAvg, double& outGlobalDark)
{
    if (!img.IsValid() || img.width <= 0 || img.height <= 0)
        return false;

    const int sourceW = (img.imageWidth > 0) ? img.imageWidth : img.width;
    const int sourceH = (img.imageHeight > 0) ? img.imageHeight : img.height;
    if (sourceW <= 0 || sourceH <= 0)
        return false;

    const WallpaperPlacement placement = MakeWallpaperPlacement(sourceW, sourceH, alignment);
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
            if (MapDisplayToSource(placement, dx + 0.5, dy + 0.5, sx, sy))
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
// Schemecolor helpers
// ============================================================
static bool parseSchemecolor3(const std::wstring& s, double& r, double& g, double& b)
{
    r = g = b = 0.0;
    int n = swscanf_s(s.c_str(), L"%lf %lf %lf", &r, &g, &b);
    if (n != 3)
        return false;
    auto clamp01 = [](double& x)
    {
        if (x < 0.0) x = 0.0;
        if (x > 1.0) x = 1.0;
    };
    clamp01(r); clamp01(g); clamp01(b);
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

static double rgbToHueDeg(double r01, double g01, double b01)
{
    const double maxv = (std::max)(r01, (std::max)(g01, b01)), minv = (std::min)(r01, (std::min)(g01, b01)),
                 d = maxv - minv;
    if (d <= 1e-12) return 0.0;
    double h = 0.0;
    if (maxv == r01)      h = (g01 - b01) / d + (g01 < b01 ? 6.0 : 0.0);
    else if (maxv == g01) h = (b01 - r01) / d + 2.0;
    else                  h = (r01 - g01) / d + 4.0;
    h *= 60.0;
    if (h < 0.0) h += 360.0;
    if (h >= 360.0) h -= 360.0;
    return h;
}

ThemeTag ClassifyFromSchemecolor(const std::wstring& sc, double)
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

} // namespace sts::we
