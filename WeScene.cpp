#include "WeScene.h"
#include "WeClassify.h"
#include "StringConvert.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>
#include <iostream>

using namespace winrt::Windows::Data::Json;

namespace sts::we
{

// Forward declarations — resolved by linker from WeConfigManager / WeClassify
std::string     utf16ToUtf8(const std::wstring& ws);
bool jsonTryGetString(JsonObject const& obj, const std::wstring& key, std::wstring& out);
bool jsonTryGetArray (JsonObject const& obj, const std::wstring& key, JsonArray& out);
bool jsonTryGetObject(JsonObject const& obj, const std::wstring& key, JsonObject& out);
bool jsonTryGetNumber(JsonObject const& obj, const std::wstring& key, double& out);
const double* GetSRGBLut();

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

static bool resolveObjectTexturePath(const PkgParser& parser, JsonObject const& obj, ResolvedTexInfo& outInfo)
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

    double cz = 0.0;
    outInfo.hasCrop = sceneTryGetVec3(model, L"cropoffset", outInfo.cropX, outInfo.cropY, cz);
    if (outInfo.hasCrop)
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
            if (resolveTexturePath(parser, dirnameUtf8(materialPath), texValue.GetString().c_str(), outInfo.path))
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

// Apply WE scene.json "colorBlendMode" to a texture pixel.
// cR/cG/cB = texture pixel (0..1), tR/tG/tB = layer color property (0..1).
// Returns the blended pixel channels (R, G, B).
// Mapping from WE scene format values to standard blend equations:
//   0 = multiply (default multiplicative tint; WE "Normal"/default behavior)
//   1 = additive / linear dodge
//   2 = subtract
//   3 = screen
//   4 = overlay
//   5 = soft light
//   6 = hard light
//   7 = color dodge
//   8 = color burn
//   9 = darken
//  10 = lighten
//  11 = difference
//  12 = multiply (empirically determined: used with grey tint on reflection layers)
// Note: the exact WE colorBlendMode mapping is not publicly documented.
// Modes 1-11 use standard blend ordering; mode 12 was determined from scene analysis.
static void applyColorBlendMode(int mode, double tR, double tG, double tB, double& cR, double& cG, double& cB)
{
    switch (mode)
    {
    case 0: // multiply (WE default multiplicative tint)
        cR *= tR;
        cG *= tG;
        cB *= tB;
        break;
    case 1: // additive / linear dodge
        cR = (std::min)(1.0, cR + tR);
        cG = (std::min)(1.0, cG + tG);
        cB = (std::min)(1.0, cB + tB);
        break;
    case 2: // subtract
        cR = (std::max)(0.0, cR - tR);
        cG = (std::max)(0.0, cG - tG);
        cB = (std::max)(0.0, cB - tB);
        break;
    case 3: // screen
        cR = 1.0 - (1.0 - cR) * (1.0 - tR);
        cG = 1.0 - (1.0 - cG) * (1.0 - tG);
        cB = 1.0 - (1.0 - cB) * (1.0 - tB);
        break;
    case 4: // overlay
    {
        auto overlayCh = [](double c, double t) {
            return (c < 0.5) ? (2.0 * c * t) : (1.0 - 2.0 * (1.0 - c) * (1.0 - t));
        };
        cR = overlayCh(cR, tR);
        cG = overlayCh(cG, tG);
        cB = overlayCh(cB, tB);
        break;
    }
    case 5: // soft light
    {
        auto softLightCh = [](double c, double t) {
            return (t < 0.5) ? (c - (1.0 - 2.0 * t) * c * (1.0 - c))
                             : (c + (2.0 * t - 1.0) * ((c <= 0.25) ? ((16.0 * c - 12.0) * c + 4.0) * c : std::sqrt(c)) - c);
        };
        cR = softLightCh(cR, tR);
        cG = softLightCh(cG, tG);
        cB = softLightCh(cB, tB);
        break;
    }
    case 6: // hard light
    {
        auto hardLightCh = [](double c, double t) {
            return (t < 0.5) ? (2.0 * c * t) : (1.0 - 2.0 * (1.0 - c) * (1.0 - t));
        };
        cR = hardLightCh(cR, tR);
        cG = hardLightCh(cG, tG);
        cB = hardLightCh(cB, tB);
        break;
    }
    case 7: // color dodge
        cR = (tR >= 1.0) ? 1.0 : (std::min)(1.0, cR / (std::max)(0.0001, 1.0 - tR));
        cG = (tG >= 1.0) ? 1.0 : (std::min)(1.0, cG / (std::max)(0.0001, 1.0 - tG));
        cB = (tB >= 1.0) ? 1.0 : (std::min)(1.0, cB / (std::max)(0.0001, 1.0 - tB));
        break;
    case 8: // color burn
        cR = (tR <= 0.0) ? 0.0 : 1.0 - (std::min)(1.0, (1.0 - cR) / (std::max)(0.0001, tR));
        cG = (tG <= 0.0) ? 0.0 : 1.0 - (std::min)(1.0, (1.0 - cG) / (std::max)(0.0001, tG));
        cB = (tB <= 0.0) ? 0.0 : 1.0 - (std::min)(1.0, (1.0 - cB) / (std::max)(0.0001, tB));
        break;
    case 9: // darken
        cR = (std::min)(cR, tR);
        cG = (std::min)(cG, tG);
        cB = (std::min)(cB, tB);
        break;
    case 10: // lighten
        cR = (std::max)(cR, tR);
        cG = (std::max)(cG, tG);
        cB = (std::max)(cB, tB);
        break;
    case 11: // difference
        cR = std::abs(cR - tR);
        cG = std::abs(cG - tG);
        cB = std::abs(cB - tB);
        break;
    case 12: // multiply (used on reflection/darkening layers in WE)
        cR *= tR;
        cG *= tG;
        cB *= tB;
        break;
    default:
        // unknown mode: fall back to multiplicative tint
        cR *= tR;
        cG *= tG;
        cB *= tB;
        break;
    }
}

static int sceneReadColorBlendMode(JsonObject const& obj)
{
    if (!obj.HasKey(L"colorBlendMode"))
        return 0; // default: multiplicative tint
    auto v = obj.GetNamedValue(L"colorBlendMode");
    if (v.ValueType() == JsonValueType::Number)
        return static_cast<int>(v.GetNumber());
    return 0;
}

bool CalcSceneCompositeStatsFromPkg(const PkgParser& parser, double wPct, double hPct,
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
    const WallpaperPlacement placement = MakeWallpaperPlacement(canvasW, canvasH, alignment,
                                                                canvasW, canvasH);
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
            const bool inside = MapDisplayToSource(placement, x + 0.5, y + 0.5, sx, sy);
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

        ResolvedTexInfo texInfo;
        if (!resolveObjectTexturePath(parser, obj, texInfo))
            continue;

        auto texIt = parser.GetVFS().find(texInfo.path);
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
        const int colorBlendMode = sceneReadColorBlendMode(obj);

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
            double sr = px[0] / 255.0;
            double sg = px[1] / 255.0;
            double sb = px[2] / 255.0;
            applyColorBlendMode(colorBlendMode, tintR, tintG, tintB, sr, sg, sb);
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

bool TestCalcSceneComposite(const std::wstring& pkgPath,
                            double wPct, double hPct,
                            double& outRoiAvg, double& outRoiDark,
                            double& outGlobalAvg, double& outGlobalDark,
                            std::wstring& outDecodeSummary)
{
    PkgParser parser;
    if (!parser.Parse(pkgPath))
        return false;
    WallpaperAlignmentSettings alignment{};
    return CalcSceneCompositeStatsFromPkg(parser, wPct, hPct, alignment,
                                          outRoiAvg, outRoiDark,
                                          outGlobalAvg, outGlobalDark,
                                          outDecodeSummary);
}

static bool writeRgbaToPngFile(const uint8_t* rgba, int w, int h, const std::wstring& outPath)
{
    winrt::com_ptr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return false;

    winrt::com_ptr<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.put())))
        return false;
    if (FAILED(stream->InitializeFromFilename(outPath.c_str(), GENERIC_WRITE)))
        return false;

    winrt::com_ptr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.put())))
        return false;
    if (FAILED(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache)))
        return false;

    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    winrt::com_ptr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(frame.put(), props.put())))
        return false;
    if (FAILED(frame->Initialize(props.get())))
        return false;
    if (FAILED(frame->SetSize(w, h)))
        return false;

    // WIC GUID_WICPixelFormat32bppRGBA stores as BGRA on Windows.
    // Our buffer is RGBA, so swap R<->B for correct PNG output.
    std::vector<uint8_t> swizzled(w * h * 4u);
    for (size_t i = 0; i < static_cast<size_t>(w) * h * 4u; i += 4)
    {
        swizzled[i]     = rgba[i + 2]; // B
        swizzled[i + 1] = rgba[i + 1]; // G
        swizzled[i + 2] = rgba[i];     // R
        swizzled[i + 3] = rgba[i + 3]; // A
    }

    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppRGBA;
    if (FAILED(frame->SetPixelFormat(&pixelFormat)))
        return false;

    // sRGB chunk
    {
        winrt::com_ptr<IWICMetadataQueryWriter> mqw;
        if (SUCCEEDED(frame->QueryInterface(IID_PPV_ARGS(mqw.put()))))
        {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            pv.vt = VT_UI1;
            pv.bVal = 0;
            mqw->SetMetadataByName(L"/sRGB/RenderingIntent", &pv);
            PropVariantClear(&pv);
        }
    }

    const UINT stride = w * 4;
    if (FAILED(frame->WritePixels(h, stride, stride * static_cast<UINT>(h),
                                  swizzled.data())))
        return false;
    if (FAILED(frame->Commit()))
        return false;
    if (FAILED(encoder->Commit()))
        return false;
    return true;
}

bool RenderSceneCompositeToPng(const std::wstring& pkgPath, const std::wstring& outPngPath,
                               double& outRoiAvg, double& outRoiDark,
                               double& outGlobalAvg, double& outGlobalDark,
                               std::wstring& outDecodeSummary,
                               const WallpaperAlignmentSettings& alignment)
{
    PkgParser parser;
    if (!parser.Parse(pkgPath))
        return false;

    // Parse scene.json
    JsonObject scene;
    if (!parseVfsJson(parser, "scene.json", scene))
        return false;

    JsonObject general, projection;
    double canvasW = 0.0, canvasH = 0.0;
    if (!jsonTryGetObject(scene, L"general", general) ||
        !jsonTryGetObject(general, L"orthogonalprojection", projection) ||
        !jsonTryGetNumber(projection, L"width", canvasW) || !jsonTryGetNumber(projection, L"height", canvasH) ||
        canvasW <= 0.0 || canvasH <= 0.0)
        return false;

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

    // Allocate full-canvas RGBA buffer
    const int cw = static_cast<int>(canvasW);
    const int ch = static_cast<int>(canvasH);
    const size_t bufSize = static_cast<size_t>(cw) * static_cast<size_t>(ch) * 4u;
    std::vector<uint8_t> buf(bufSize);

    // Init with clear color
    const uint8_t cr = static_cast<uint8_t>((std::max)(0.0, (std::min)(1.0, clearR)) * 255.0 + 0.5);
    const uint8_t cg = static_cast<uint8_t>((std::max)(0.0, (std::min)(1.0, clearG)) * 255.0 + 0.5);
    const uint8_t cb = static_cast<uint8_t>((std::max)(0.0, (std::min)(1.0, clearB)) * 255.0 + 0.5);
    for (size_t i = 0; i < bufSize; i += 4)
    {
        buf[i + 0] = cr;
        buf[i + 1] = cg;
        buf[i + 2] = cb;
        buf[i + 3] = 255;
    }

    // Composite layers back-to-front
    int decodedLayerCount = 0;
    int skippedLayerCount = 0;
    int skippedNotVisible = 0;
    int skippedNoSize = 0;
    int skippedOffCanvas = 0;
    int skippedNoClamp = 0;
    int skippedNoTexPath = 0;
    int skippedTexNotFound = 0;
    int skippedTexDecodeFail = 0;
    int solidLayerCount = 0;
    std::vector<std::wstring> decodeFormats;

    auto rememberFormat = [&](const std::wstring& fmt)
    {
        if (fmt.empty()) return;
        for (const auto& existing : decodeFormats)
            if (existing == fmt) return;
        decodeFormats.push_back(fmt);
    };

    std::wcout << L"[render] Canvas: " << cw << L"x" << ch
               << L"  clearColor=(" << clearR << L"," << clearG << L"," << clearB << L")"
               << L"  layers=" << objects.Size() << std::endl;

    for (uint32_t i = 0; i < objects.Size(); ++i)
    {
        if (objects.GetAt(i).ValueType() != JsonValueType::Object)
            continue;
        JsonObject obj = objects.GetAt(i).GetObject();
        if (!sceneObjectVisible(obj))
        {
            skippedLayerCount++;
            skippedNotVisible++;
            if (skippedNotVisible <= 5) {
                std::wstring img;
                jsonTryGetString(obj, L"image", img);
                size_t ls = img.find_last_of(L'/');
                if (ls != std::wstring::npos) img = img.substr(ls + 1);
                std::wcout << L"  [SKIP-notVisible] " << img << std::endl;
            }
            continue;
        }

        double originX = 0.0, originY = 0.0, originZ = 0.0;
        sceneAbsoluteOrigin(obj, objectsById, originX, originY, originZ);

        // Read image path early for composelayer detection
        std::wstring imagePathW;
        jsonTryGetString(obj, L"image", imagePathW);
        bool currentIsCompose = (imagePathW.find(L"models/util/composelayer.json") != std::wstring::npos);

        double scaleX = 1.0, scaleY = 1.0, scaleZ = 1.0;
        sceneTryGetVec3(obj, L"scale", scaleX, scaleY, scaleZ);
        double angleX = 0.0, angleY = 0.0, angleZ = 0.0;
        sceneTryGetVec3(obj, L"angles", angleX, angleY, angleZ);
        // Determine texture flip from negative scale sign and ~180 deg Z rotation.
        bool texFlipU = (scaleX < 0.0);
        bool texFlipV = (scaleY < 0.0);
        if (std::abs(std::abs(angleZ) - 3.141592653589793) < 0.1) {
            texFlipU = !texFlipU;
            texFlipV = !texFlipV;
        }

        double sizeX = 0.0, sizeY = 0.0, sizeZ = 0.0;
        sceneTryGetVec3(obj, L"size", sizeX, sizeY, sizeZ);
        if (sizeX <= 0.0 || sizeY <= 0.0)
        {
            skippedLayerCount++;
            skippedNoSize++;
            if (skippedNoSize <= 10) {
                std::wstring img;
                jsonTryGetString(obj, L"image", img);
                size_t ls = img.find_last_of(L'/');
                if (ls != std::wstring::npos) img = img.substr(ls + 1);
                double cr=0,cg=0,cb=0;
                sceneTryGetVec3(obj, L"color", cr, cg, cb);
                std::wcout << L"  [SKIP-noSize] " << img << L" color=(" << cr << L"," << cg << L"," << cb << L")" << std::endl;
            }
            continue;
        }

        const double drawW = sizeX * std::abs(scaleX);
        const double drawH = sizeY * std::abs(scaleY);
        if (drawW <= 1.0 || drawH <= 1.0)
        {
            skippedLayerCount++;
            skippedNoSize++;
            continue;
        }

        const double left = originX - drawW * 0.5;
        const double top = originY - drawH * 0.5;
        const double right = left + drawW;
        const double bottom = top + drawH;
        if (right < 0.0 || left > canvasW || bottom < 0.0 || top > canvasH)
        {
            skippedLayerCount++;
            skippedOffCanvas++;
            continue;
        }

        // Clamp bounding box to canvas
        const int bx0 = (std::max)(0, static_cast<int>(left));
        const int by0 = (std::max)(0, static_cast<int>(top));
        const int bx1 = (std::min)(cw, static_cast<int>(right + 0.999999));
        const int by1 = (std::min)(ch, static_cast<int>(bottom + 0.999999));
        if (bx0 >= bx1 || by0 >= by1)
        {
            skippedLayerCount++;
            skippedNoClamp++;
            continue;
        }

        const double objectAlpha = (std::max)(0.0, (std::min)(1.0, sceneReadScalar(obj, L"alpha", 1.0)));

        // Scan effect passes for color constants (e.g. audio bars "Bar Color")
        auto tryExtractEffectColor = [](JsonObject const& o) -> std::optional<std::tuple<double,double,double>> {
            JsonArray effects;
            if (!jsonTryGetArray(o, L"effects", effects)) return std::nullopt;
            for (uint32_t ei = 0; ei < effects.Size(); ++ei) {
                if (effects.GetAt(ei).ValueType() != JsonValueType::Object) continue;
                JsonObject eff = effects.GetAt(ei).GetObject();
                JsonArray passes;
                if (!jsonTryGetArray(eff, L"passes", passes)) continue;
                for (uint32_t pi = 0; pi < passes.Size(); ++pi) {
                    if (passes.GetAt(pi).ValueType() != JsonValueType::Object) continue;
                    JsonObject pass = passes.GetAt(pi).GetObject();
                    JsonObject csv;
                    if (!jsonTryGetObject(pass, L"constantshadervalues", csv)) continue;
                    const wchar_t* colorKeys[] = {L"Bar Color", L"Color", L"color", L"Tint", L"tint",
                                                  L"Blend Color", L"blendcolor"};
                    for (auto ck : colorKeys) {
                        if (!csv.HasKey(ck)) continue;
                        auto val = csv.GetNamedValue(ck);
                        if (val.ValueType() != JsonValueType::String) continue;
                        std::wstring s = val.GetString().c_str();
                        double r=1,g=1,b=1;
                        if (swscanf_s(s.c_str(), L"%lf %lf %lf", &r, &g, &b) >= 2)
                            return std::make_tuple(
                                (std::max)(0.0,(std::min)(1.0,r)),
                                (std::max)(0.0,(std::min)(1.0,g)),
                                (std::max)(0.0,(std::min)(1.0,b)));
                    }
                }
            }
            return std::nullopt;
        };

        // Solid layer
        bool isSolid = (imagePathW.find(L"models/util/solidlayer.json") != std::wstring::npos);
        bool isEmptyA = (imagePathW == L"A" || imagePathW.empty() ||
                         (imagePathW.size() == 1 && imagePathW[0] == L'A'));
        // Empty image with a "text" property is a text layer, not a solid color rectangle.
        // We can't render text yet, so skip it rather than drawing a wrong-colored block.
        if (isEmptyA && obj.HasKey(L"text"))
        {
            skippedLayerCount++;
            skippedNoTexPath++;
            continue;
        }
        if (isSolid || isEmptyA)
        {
            // Skip solidlayers with effects (audio visualizers, shader effects, etc.)
            // – we can't reproduce dynamic visuals as a static colored rectangle.
            JsonArray solidEffects;
            if (isSolid && jsonTryGetArray(obj, L"effects", solidEffects) && solidEffects.Size() > 0)
            {
                skippedLayerCount++;
                skippedNoTexPath++;
                continue;
            }

            double sr = 1.0, sg = 1.0, sb = 1.0;
            sceneTryGetVec3(obj, L"color", sr, sg, sb);
            // If no explicit color, try to extract from effect constants (e.g. audio bars "Bar Color")
            if (sr == 1.0 && sg == 1.0 && sb == 1.0) {
                auto effCol = tryExtractEffectColor(obj);
                if (effCol) {
                    sr = std::get<0>(*effCol);
                    sg = std::get<1>(*effCol);
                    sb = std::get<2>(*effCol);
                }
            }
            const uint8_t srb = static_cast<uint8_t>((std::max)(0.0, (std::min)(1.0, sr)) * 255.0 + 0.5);
            const uint8_t sgb = static_cast<uint8_t>((std::max)(0.0, (std::min)(1.0, sg)) * 255.0 + 0.5);
            const uint8_t sbb = static_cast<uint8_t>((std::max)(0.0, (std::min)(1.0, sb)) * 255.0 + 0.5);
            const uint8_t aByte = static_cast<uint8_t>(objectAlpha * 255.0 + 0.5);
            const double aNorm = objectAlpha;
            const double invA = 1.0 - aNorm;

            for (int py = by0; py < by1; ++py)
            {
                size_t rowStart = (static_cast<size_t>(py) * cw + bx0) * 4u;
                for (int px = bx0; px < bx1; ++px, rowStart += 4)
                {
                    buf[rowStart + 0] = static_cast<uint8_t>(srb * aNorm + buf[rowStart + 0] * invA + 0.5);
                    buf[rowStart + 1] = static_cast<uint8_t>(sgb * aNorm + buf[rowStart + 1] * invA + 0.5);
                    buf[rowStart + 2] = static_cast<uint8_t>(sbb * aNorm + buf[rowStart + 2] * invA + 0.5);
                    // alpha channel stays 255 (fully opaque background)
                }
            }
            decodedLayerCount++;
            solidLayerCount++;
            continue;
        }

        // Textured layer
        ResolvedTexInfo texInfo;
        if (!resolveObjectTexturePath(parser, obj, texInfo))
        {
            skippedLayerCount++;
            skippedNoTexPath++;
            continue;
        }

        auto texIt = parser.GetVFS().find(texInfo.path);
        if (texIt == parser.GetVFS().end())
        {
            skippedLayerCount++;
            skippedTexNotFound++;
            continue;
        }

        PkgParser::RgbaImage img = parser.DecodeTexvToRGBA(texIt->second);
        if (!img.IsValid())
        {
            skippedLayerCount++;
            skippedTexDecodeFail++;
            continue;
        }

        const int imageW = (img.imageWidth > 0) ? img.imageWidth : img.width;
        const int imageH = (img.imageHeight > 0) ? img.imageHeight : img.height;
        if (imageW <= 0 || imageH <= 0)
            continue;

        double tintR = 1.0, tintG = 1.0, tintB = 1.0;
        sceneTryGetVec3(obj, L"color", tintR, tintG, tintB);
        const int colorBlendMode = sceneReadColorBlendMode(obj);

        for (int py = by0; py < by1; ++py)
        {
            const double vRaw = (static_cast<double>(py) + 0.5 - top) / drawH;
            const double v = texFlipV ? (1.0 - vRaw) : vRaw;
            const int ty = (std::max)(0, (std::min)(imageH - 1, static_cast<int>(v * imageH)));
            const uint8_t* texRow = img.pixels.data() + (static_cast<size_t>(ty) * static_cast<size_t>(img.width)) * 4u;
            size_t rowStart = (static_cast<size_t>(py) * cw + bx0) * 4u;

            for (int px = bx0; px < bx1; ++px, rowStart += 4)
            {
                const double uRaw = (static_cast<double>(px) + 0.5 - left) / drawW;
                const double u = texFlipU ? (1.0 - uRaw) : uRaw;
                const int tx = (std::max)(0, (std::min)(imageW - 1, static_cast<int>(u * imageW)));
                const uint8_t* txPx = texRow + static_cast<size_t>(tx) * 4u;

                const double alpha = (txPx[3] / 255.0) * objectAlpha;
                if (alpha <= 0.001)
                    continue;

                double sr = txPx[0] / 255.0;
                double sg = txPx[1] / 255.0;
                double sb = txPx[2] / 255.0;
                applyColorBlendMode(colorBlendMode, tintR, tintG, tintB, sr, sg, sb);

                // Blend mode 12 (multiply) multiplies source with destination,
                // unlike normal alpha blending which just overlays the source.
                if (colorBlendMode == 12) {
                    sr *= (buf[rowStart + 0] / 255.0);
                    sg *= (buf[rowStart + 1] / 255.0);
                    sb *= (buf[rowStart + 2] / 255.0);
                }

                const double invA = 1.0 - alpha;
                buf[rowStart + 0] = static_cast<uint8_t>(sr * 255.0 * alpha + buf[rowStart + 0] * invA + 0.5);
                buf[rowStart + 1] = static_cast<uint8_t>(sg * 255.0 * alpha + buf[rowStart + 1] * invA + 0.5);
                buf[rowStart + 2] = static_cast<uint8_t>(sb * 255.0 * alpha + buf[rowStart + 2] * invA + 0.5);
            }
        }

        decodedLayerCount++;
        rememberFormat(img.decodeFormat);
    }

    if (decodedLayerCount == 0)
    {
        std::wcout << L"[render] ERROR: 0 layers decoded! skipped=" << skippedLayerCount
                   << L" total=" << objects.Size() << std::endl;
        return false;
    }

    std::wcout << L"[render] Layers: decoded=" << decodedLayerCount
               << L" solid=" << solidLayerCount
               << L" textured=" << (decodedLayerCount - solidLayerCount)
               << L" skipped=" << skippedLayerCount
               << L" total=" << objects.Size() << std::endl;
    std::wcout << L"[render] Skip reasons: notVisible=" << skippedNotVisible
               << L" noSize=" << skippedNoSize
               << L" offCanvas=" << skippedOffCanvas
               << L" noClamp=" << skippedNoClamp
               << L" noTexPath=" << skippedNoTexPath
               << L" texNotFound=" << skippedTexNotFound
               << L" texDecodeFail=" << skippedTexDecodeFail
               << std::endl;

    // Save as PNG and calculate stats
    const double* lut = GetSRGBLut();
    const uint8_t* outBuf = buf.data();
    int outW = cw;
    int outH = ch;
    std::vector<uint8_t> croppedBuf;

    if (alignment.custom)
    {
        const WallpaperPlacement placement = MakeWallpaperPlacement(canvasW, canvasH, alignment);

        // Compute visible canvas region directly from placement math.
        // displayX = contentX + (sourceX / sourceW) * contentW
        // sourceX = (displayX - contentX) / contentW * sourceW
        // Clamp the four display-edge source coordinates to canvas bounds.
        const double srcW = placement.sourceW;
        const double srcH = placement.sourceH;
        const double cX = placement.contentX;
        const double cY = placement.contentY;
        const double cW = placement.contentW;
        const double cH = placement.contentH;
        const double dW = placement.displayW;
        const double dH = placement.displayH;

        auto dispToSrcX = [&](double dx) { return ((dx - cX) / cW) * srcW; };
        auto dispToSrcY = [&](double dy) { return ((dy - cY) / cH) * srcH; };

        const double sx0 = dispToSrcX(0.0);
        const double sx1 = dispToSrcX(dW);
        const double sy0 = dispToSrcY(0.0);
        const double sy1 = dispToSrcY(dH);

        const int cropX0 = (std::max)(0,            static_cast<int>(std::ceil((std::max)(0.0, (std::min)(sx0, sx1)))));
        const int cropY0 = (std::max)(0,            static_cast<int>(std::ceil((std::max)(0.0, (std::min)(sy0, sy1)))));
        const int cropX1 = (std::min)(cw - 1,       static_cast<int>(std::floor((std::min)(srcW - 1.0, (std::max)(sx0, sx1)))));
        const int cropY1 = (std::min)(ch - 1,       static_cast<int>(std::floor((std::min)(srcH - 1.0, (std::max)(sy0, sy1)))));

        if (cropX1 > cropX0 && cropY1 > cropY0)
        {
            outW = cropX1 - cropX0 + 1;
            outH = cropY1 - cropY0 + 1;
            croppedBuf.resize(static_cast<size_t>(outW) * outH * 4u);
            for (int cy = cropY0; cy <= cropY1; ++cy)
            {
                const size_t srcRow = static_cast<size_t>(cy) * cw * 4u;
                const size_t dstRow = static_cast<size_t>(cy - cropY0) * outW * 4u;
                for (int cx = cropX0; cx <= cropX1; ++cx)
                {
                    const size_t srcOff = srcRow + static_cast<size_t>(cx) * 4u;
                    const size_t dstOff = dstRow + static_cast<size_t>(cx - cropX0) * 4u;
                    croppedBuf[dstOff + 0] = buf[srcOff + 0];
                    croppedBuf[dstOff + 1] = buf[srcOff + 1];
                    croppedBuf[dstOff + 2] = buf[srcOff + 2];
                    croppedBuf[dstOff + 3] = buf[srcOff + 3];
                }
            }
            outBuf = croppedBuf.data();
            std::wcout << L"[render] Alignment crop: canvas(" << cropX0 << L"," << cropY0
                       << L")-(" << cropX1 << L"," << cropY1
                       << L") -> " << outW << L"x" << outH << std::endl;
        }
    }

    if (!writeRgbaToPngFile(outBuf, outW, outH, outPngPath))
        return false;

    // Calculate stats on output region
    double globalSum = 0.0;
    int globalDark = 0;
    const size_t totalPixels = static_cast<size_t>(outW) * outH;
    for (size_t i = 0; i < totalPixels; ++i)
    {
        const size_t off = i * 4u;
        const int r = outBuf[off + 0], g = outBuf[off + 1], b = outBuf[off + 2];
        const double L = 0.2126 * lut[r] + 0.7152 * lut[g] + 0.0722 * lut[b];
        globalSum += L;
        if (L < 0.179) ++globalDark;
    }
    outGlobalAvg = globalSum / static_cast<double>(totalPixels);
    outGlobalDark = static_cast<double>(globalDark) / static_cast<double>(totalPixels);

    // ROI stats: sample display space at tray position, using clearColor
    // luminance for areas not covered by the alignment.
    if (alignment.custom)
    {
        const double wPct = 0.25;
        const double hPct = 0.08;
        const WallpaperPlacement placement = MakeWallpaperPlacement(canvasW, canvasH, alignment,
                                                                    canvasW, canvasH);
        const int dispW = (std::max)(1, static_cast<int>(placement.displayW + 0.5));
        const int dispH = (std::max)(1, static_cast<int>(placement.displayH + 0.5));
        const int roiW = (std::max)(1, static_cast<int>(placement.displayW * wPct));
        const int roiH = (std::max)(1, static_cast<int>(placement.displayH * hPct));
        const int roiX = (std::max)(0, dispW - roiW);
        const int roiY = (std::max)(0, dispH - roiH);
        const int step = 4;

        const double clearL = 0.2126 * lut[static_cast<int>(clearR * 255.0 + 0.5)]
                            + 0.7152 * lut[static_cast<int>(clearG * 255.0 + 0.5)]
                            + 0.0722 * lut[static_cast<int>(clearB * 255.0 + 0.5)];

        double roiSum = 0.0;
        int roiDarkCount = 0;
        int roiSamples = 0;
        for (int y = roiY; y < dispH; y += step)
        {
            for (int x = roiX; x < dispW; x += step)
            {
                double sx = 0.0, sy = 0.0;
                double L = clearL;
                if (MapDisplayToSource(placement, x + 0.5, y + 0.5, sx, sy))
                {
                    const int cx = static_cast<int>(sx);
                    const int cy = static_cast<int>(sy);
                    if (cx >= 0 && cx < cw && cy >= 0 && cy < ch)
                    {
                        const size_t off = (static_cast<size_t>(cy) * cw + cx) * 4u;
                        const int r = buf[off + 0], g = buf[off + 1], b = buf[off + 2];
                        L = 0.2126 * lut[r] + 0.7152 * lut[g] + 0.0722 * lut[b];
                    }
                }
                roiSum += L;
                if (L < 0.179) ++roiDarkCount;
                ++roiSamples;
            }
        }
        if (roiSamples > 0)
        {
            outRoiAvg = roiSum / static_cast<double>(roiSamples);
            outRoiDark = static_cast<double>(roiDarkCount) / static_cast<double>(roiSamples);
        }
        else
        {
            outRoiAvg = outGlobalAvg;
            outRoiDark = outGlobalDark;
        }
    }
    else
    {
        outRoiAvg = outGlobalAvg;
        outRoiDark = outGlobalDark;
    }

    // Build decode summary
    outDecodeSummary.clear();
    for (size_t i = 0; i < decodeFormats.size(); ++i)
    {
        if (i != 0) outDecodeSummary += L"+";
        outDecodeSummary += decodeFormats[i];
    }
    if (outDecodeSummary.empty())
        outDecodeSummary = L"solid";
    return true;
}

bool RenderBackgroundMediaToPng(const std::wstring& pkgPath, const std::wstring& outPngPath,
                                double wPct, double hPct,
                                double& outRoiAvg, double& outRoiDark,
                                double& outGlobalAvg, double& outGlobalDark,
                                std::wstring& outDecodeSummary)
{
    PkgParser parser;
    if (!parser.Parse(pkgPath))
        return false;

    std::string bgMedia = parser.FindBackgroundMedia();
    if (bgMedia.empty())
        return false;

    auto it = parser.GetVFS().find(bgMedia);
    if (it == parser.GetVFS().end())
        return false;

    PkgParser::RgbaImage img = parser.DecodeTexvToRGBA(it->second);
    if (!img.IsValid())
        return false;

    outDecodeSummary = L"bg:" + sts::WStringFromUtf8(bgMedia) + L" " + img.decodeFormat;

    if (!parser.CalcStatsFromRgba(img, wPct, hPct, outRoiAvg, outRoiDark, outGlobalAvg, outGlobalDark))
        return false;

    const int w = (img.imageWidth > 0) ? img.imageWidth : img.width;
    const int h = (img.imageHeight > 0) ? img.imageHeight : img.height;
    return writeRgbaToPngFile(img.pixels.data(), w, h, outPngPath);
}

} // namespace sts::we
