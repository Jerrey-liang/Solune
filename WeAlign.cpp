#include "WeAlign.h"
#include <winrt/Windows.Foundation.Collections.h>
#include <algorithm>

namespace sts::we
{

// Forward-declared from WeConfigManager.cpp (json helpers)
bool jsonTryGetNumber(JsonObject const& obj, const std::wstring& key, double& out);
bool jsonTryGetBool(JsonObject const& obj, const std::wstring& key, bool& out);

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

WallpaperAlignmentSettings ReadWallpaperAlignment(JsonObject const& monitor0)
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
    a.z = (std::max)(1.0, (std::min)(400.0, a.z));
    return a;
}

WallpaperPlacement MakeWallpaperPlacement(double sourceW, double sourceH, const WallpaperAlignmentSettings& align)
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

    const double zoom = align.z / 100.0;
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
        case 4: // Free — unit-based: (50,50)=center, 1 unit = 1% of source width
            {
                const double unitPx = sourceW * 0.01;
                p.contentW = sourceW * zoom;
                p.contentH = sourceH * zoom;
                p.contentX = (p.displayW - p.contentW) * 0.5 - (50.0 - align.x) * unitPx;
                p.contentY = (p.displayH - p.contentH) * 0.5 + (align.y - 50.0) * unitPx;
            }
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

bool MapDisplayToSource(const WallpaperPlacement& p, double displayX, double displayY, double& outX, double& outY)
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

} // namespace sts::we
