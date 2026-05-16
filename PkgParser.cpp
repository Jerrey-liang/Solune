#include "PkgParser.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

#include "lz4.h"
#include <cstring>
#include <string_view>

#define BCDEC_IMPLEMENTATION
#include "bcdec.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "StringConvert.h"

static uint32_t ReadLeU32(const uint8_t* ptr)
{
    uint32_t value = 0;
    std::memcpy(&value, ptr, sizeof(value));
    return value;
}

static const wchar_t* TexFormatName(int32_t format)
{
    switch (format)
    {
        case 0:
            return L"RGBA8888";
        case 4:
            return L"DXT5/BC3";
        case 6:
            return L"DXT3/BC2";
        case 7:
            return L"DXT1/BC1";
        case 8:
            return L"RG88";
        case 9:
            return L"R8";
        default:
            return L"Unsupported";
    }
}

PkgParser::PkgParser() = default;

PkgParser::~PkgParser()
{
    Cleanup();
}

void PkgParser::Cleanup()
{
    if (m_basePtr)
    {
        UnmapViewOfFile(m_basePtr);
        m_basePtr = nullptr;
    }
    if (m_hMapping)
    {
        CloseHandle(m_hMapping);
        m_hMapping = nullptr;
    }
    if (m_hFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_hFile);
        m_hFile = INVALID_HANDLE_VALUE;
    }
    m_fileSize = 0;
    m_vfs.clear();
}

bool PkgParser::FailAndCleanup(const std::wstring& errorMsg)
{
#ifdef _DEBUG
    std::wcerr << L"[PkgParser] 错误: " << errorMsg << std::endl;
#endif
    Cleanup();
    return false;
}

bool PkgParser::Parse(const std::wstring& filePath)
{
    Cleanup();

    m_hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m_hFile == INVALID_HANDLE_VALUE)
        return FailAndCleanup(L"无法打开文件");

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(m_hFile, &fileSize))
        return FailAndCleanup(L"无法获取文件大小");
    m_fileSize = static_cast<size_t>(fileSize.QuadPart);
    if (m_fileSize < 12)
        return FailAndCleanup(L"文件过小");

    m_hMapping = CreateFileMappingW(m_hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m_hMapping)
        return FailAndCleanup(L"创建内存映射失败");

    m_basePtr = static_cast<const uint8_t*>(MapViewOfFile(m_hMapping, FILE_MAP_READ, 0, 0, 0));
    if (!m_basePtr)
        return FailAndCleanup(L"映射内存视图失败");

    const uint8_t* cursor = m_basePtr;
    const uint8_t* const endPtr = m_basePtr + m_fileSize;
    if (cursor + sizeof(uint32_t) > endPtr)
        return FailAndCleanup(L"无法读取魔数长度");

    const uint32_t magicLen = ReadLeU32(cursor);
    cursor += sizeof(uint32_t);
    if (magicLen == 0 || magicLen > 256 || cursor + magicLen > endPtr)
        return FailAndCleanup(L"魔数长度异常，不是标准的 PKG 文件");

    std::string versionStr(reinterpret_cast<const char*>(cursor), magicLen);
    cursor += magicLen;
    if (versionStr.size() < 4 || std::strncmp(versionStr.c_str(), "PKGV", 4) != 0)
        return FailAndCleanup(L"魔数校验失败，实际读取到: " + sts::WStringFromUtf8(versionStr));

    if (cursor + sizeof(uint32_t) > endPtr)
        return FailAndCleanup(L"无法读取文件数量");
    const uint32_t fileCount = ReadLeU32(cursor);
    cursor += sizeof(uint32_t);
    if (fileCount > 500000)
        return FailAndCleanup(L"文件数量过多，拒绝解析防内存溢出");

    struct EntryInfo
    {
        std::string name;
        uint32_t offset = 0;
        uint32_t length = 0;
    };

    std::vector<EntryInfo> entries;
    entries.reserve(fileCount);
    m_vfs.reserve(static_cast<size_t>(fileCount) << 1);

    for (uint32_t i = 0; i < fileCount; ++i)
    {
        if (cursor + sizeof(uint32_t) > endPtr)
            return FailAndCleanup(L"TOC 越界");
        const uint32_t nameLen = ReadLeU32(cursor);
        cursor += sizeof(uint32_t);
        if (nameLen > 2048 || cursor + nameLen > endPtr)
            return FailAndCleanup(L"TOC 文件名长度异常");

        EntryInfo entry;
        entry.name.assign(reinterpret_cast<const char*>(cursor), nameLen);
        cursor += nameLen;

        if (cursor + (sizeof(uint32_t) << 1) > endPtr)
            return FailAndCleanup(L"TOC 越界");
        entry.offset = ReadLeU32(cursor);
        entry.length = ReadLeU32(cursor + sizeof(uint32_t));
        cursor += (sizeof(uint32_t) << 1);
        entries.emplace_back(std::move(entry));
    }

    const uint8_t* const dataBlockBase = cursor;
    const size_t dataBlockSize = static_cast<size_t>(endPtr - dataBlockBase);
    for (const auto& entry : entries)
    {
        const size_t endOffset = static_cast<size_t>(entry.offset) + static_cast<size_t>(entry.length);
        if (endOffset > dataBlockSize)
            continue;
        m_vfs.emplace(entry.name, MemSpan{dataBlockBase + entry.offset, entry.length});
    }

    return true;
}

bool PkgParser::IsReferencedByJsons(const std::string& targetBaseName) const
{
    for (const auto& [path, span] : m_vfs)
    {
        if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".json") == 0)
        {
            const std::string_view jsonView(reinterpret_cast<const char*>(span.data), span.size);
            if (jsonView.find(targetBaseName) != std::string_view::npos)
                return true;
        }
    }
    return false;
}

float PkgParser::CalculateBaseScore(const std::string& path, size_t fileSize) const
{
    float score = 0.0f;
    std::string lowerPath = path;
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                   [](unsigned char c)
                   {
                       return static_cast<char>(std::tolower(c));
                   });

    if (lowerPath.find("mask") != std::string::npos || lowerPath.find("particle") != std::string::npos ||
        lowerPath.find("effect") != std::string::npos || lowerPath.find("ui") != std::string::npos)
    {
        score -= 50.0f;
    }

    if (lowerPath.find("bg") != std::string::npos || lowerPath.find("background") != std::string::npos ||
        lowerPath.find(u8"背景") != std::string::npos)
    {
        score += 20.0f;
    }

    score += static_cast<float>(fileSize) / (1024.0f * 1024.0f);
    return score;
}

std::string PkgParser::FindBackgroundMedia() const
{
    struct Candidate
    {
        std::string path;
        std::string baseName;
        size_t fileSize = 0;
        float baseScore = 0.0f;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(m_vfs.size());

    for (const auto& [path, span] : m_vfs)
    {
        if (path.size() < 4)
            continue;

        const auto lowerEndsWith = [&](const char* suffix, size_t len)
        {
            if (path.size() < len)
                return false;
            const size_t start = path.size() - len;
            for (size_t i = 0; i < len; ++i)
            {
                const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(path[start + i])));
                if (a != suffix[i])
                    return false;
            }
            return true;
        };

        if (!lowerEndsWith(".tex", 4) && !lowerEndsWith(".mp4", 4) && !lowerEndsWith(".png", 4) &&
            !lowerEndsWith(".jpg", 4))
        {
            continue;
        }

        const size_t slashPos = path.find_last_of('/');
        const std::string fileName = (slashPos == std::string::npos) ? path : path.substr(slashPos + 1);
        const size_t dotPos = fileName.find_last_of('.');
        candidates.push_back(Candidate{path, fileName.substr(0, dotPos), span.size, CalculateBaseScore(path, span.size)});
    }

    if (candidates.empty())
        return {};

    std::vector<std::string_view> jsonViews;
    jsonViews.reserve(m_vfs.size() >> 3);
    for (const auto& [path, span] : m_vfs)
    {
        if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".json") == 0)
            jsonViews.emplace_back(reinterpret_cast<const char*>(span.data), span.size);
    }

    std::string bestCandidate;
    float highestScore = -9999.0f;
    for (const auto& candidate : candidates)
    {
        float score = candidate.baseScore;
        bool referenced = false;
        for (const auto& jsonView : jsonViews)
        {
            if (jsonView.find(candidate.baseName) != std::string_view::npos)
            {
                referenced = true;
                break;
            }
        }
        score += referenced ? 50.0f : -10.0f;
        if (score > highestScore)
        {
            highestScore = score;
            bestCandidate = candidate.path;
        }
    }

    return bestCandidate;
}

MemSpan PkgParser::ExtractRawMediaFromTex(const MemSpan& texSpan) const
{
    if (texSpan.size < 8 || texSpan.data == nullptr)
        return {nullptr, 0};

    const bool isContainer = (std::strncmp(reinterpret_cast<const char*>(texSpan.data), "TEXB", 4) == 0 ||
                              std::strncmp(reinterpret_cast<const char*>(texSpan.data), "TEXV", 4) == 0);
    const size_t scanLimit = (std::min<size_t>)(texSpan.size, 2048);

    auto findMagic = [&](const uint8_t* magic, size_t magicSize) -> const uint8_t*
    {
        const uint8_t* const end = texSpan.data + scanLimit - magicSize + 1;
        for (const uint8_t* p = texSpan.data; p < end; ++p)
        {
            if (std::memcmp(p, magic, magicSize) == 0)
                return p;
        }
        return nullptr;
    };

    static constexpr uint8_t pngMagic[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    if (const uint8_t* p = findMagic(pngMagic, sizeof(pngMagic)))
        return {p, static_cast<size_t>(texSpan.data + texSpan.size - p)};

    static constexpr uint8_t jpegMagic[3] = {0xFF, 0xD8, 0xFF};
    if (const uint8_t* p = findMagic(jpegMagic, sizeof(jpegMagic)))
        return {p, static_cast<size_t>(texSpan.data + texSpan.size - p)};

    static constexpr uint8_t mp4Magic[4] = {'f', 't', 'y', 'p'};
    if (const uint8_t* p = findMagic(mp4Magic, sizeof(mp4Magic)))
    {
        const ptrdiff_t ftypOffset = p - texSpan.data;
        const ptrdiff_t startOffset = (ftypOffset >= 4) ? (ftypOffset - 4) : 0;
        return {texSpan.data + startOffset, static_cast<size_t>(texSpan.size - startOffset)};
    }

    if (!isContainer)
        return {nullptr, 0};
    return {nullptr, 0};
}

PkgParser::RgbaImage PkgParser::DecodeTexvToRGBA(const MemSpan& texSpan) const
{
    RgbaImage result;
    auto fail = [&]() -> RgbaImage
    {
        result.width = 0;
        result.height = 0;
        result.imageWidth = 0;
        result.imageHeight = 0;
        result.sourceFormat = -1;
        result.decodeFormat.clear();
        result.pixels.clear();
        return result;
    };

    if (texSpan.size < sizeof(TexvHeader) || texSpan.data == nullptr)
        return fail();

    const auto* header = reinterpret_cast<const TexvHeader*>(texSpan.data);
    if (std::strncmp(header->magic1, "TEXV", 4) != 0)
        return fail();

    result.width = header->textureWidth;
    result.height = header->textureHeight;
    result.imageWidth = (header->imageWidth > 0 && header->imageWidth <= header->textureWidth) ? header->imageWidth
                                                                                                : header->textureWidth;
    result.imageHeight = (header->imageHeight > 0 && header->imageHeight <= header->textureHeight)
                             ? header->imageHeight
                             : header->textureHeight;
    result.sourceFormat = header->format;
    result.decodeFormat = TexFormatName(header->format);

    const bool supportedFormat =
        header->format == 0 || header->format == 4 || header->format == 6 || header->format == 7 ||
        header->format == 8 || header->format == 9;
    if (result.width <= 0 || result.height <= 0 || result.width > 8192 || result.height > 8192 || !supportedFormat)
    {
        return fail();
    }

    const uint32_t widthSignature = static_cast<uint32_t>(result.width);
    const uint32_t heightSignature = static_cast<uint32_t>(result.height);
    std::array<uint8_t, sizeof(uint32_t) * 2> mipmapSignature{};
    std::memcpy(mipmapSignature.data(), &widthSignature, sizeof(widthSignature));
    std::memcpy(mipmapSignature.data() + sizeof(widthSignature), &heightSignature, sizeof(heightSignature));

    const uint8_t* const searchStart = texSpan.data + sizeof(TexvHeader);
    // flags != 0 means TEXB container — data is deeper, need larger search window
    const size_t searchLimit = (header->flags != 0) ? 65536 : 4096;
    const uint8_t* const searchEnd = searchStart + (std::min<size_t>)(texSpan.size - sizeof(TexvHeader), searchLimit);
    const uint8_t* it = nullptr;
    for (const uint8_t* p = searchStart; p + mipmapSignature.size() <= searchEnd; ++p)
    {
        if (std::memcmp(p, mipmapSignature.data(), mipmapSignature.size()) == 0)
        {
            it = p;
            break;
        }
    }
    if (!it)
    {
        // TEXB container: LZ4-compressed mipmap data — try decompress then search
        const uint8_t* texbStart = searchStart;
        while (texbStart + 9 <= searchEnd && std::strncmp(reinterpret_cast<const char*>(texbStart), "TEXB", 4) != 0)
            ++texbStart;
        if (texbStart + 9 <= searchEnd && std::strncmp(reinterpret_cast<const char*>(texbStart), "TEXB", 4) == 0)
        {
            const uint8_t* lz4Hdr = texbStart + 9; // skip "TEXB0004\0"
            if (lz4Hdr + 12 <= texSpan.data + texSpan.size)
            {
                const uint32_t lz4Flag = ReadLeU32(lz4Hdr);
                const uint32_t dSize = ReadLeU32(lz4Hdr + 4);
                const uint32_t cSize = ReadLeU32(lz4Hdr + 8);
                const uint8_t* cData = lz4Hdr + 12;
                if (lz4Flag == 1 && cSize > 0 && dSize > 0 &&
                    static_cast<size_t>(cSize) <= static_cast<size_t>(texSpan.size - (cData - texSpan.data)))
                {
                    std::vector<uint8_t> decomp(dSize);
                    int r = LZ4_decompress_safe(reinterpret_cast<const char*>(cData),
                                                reinterpret_cast<char*>(decomp.data()), cSize, dSize);
                    if (r == static_cast<int>(dSize))
                    {
                        // Search for mipmap signature in decompressed data
                        for (const uint8_t* dp = decomp.data(); dp + mipmapSignature.size() <= decomp.data() + dSize; ++dp)
                        {
                            if (std::memcmp(dp, mipmapSignature.data(), mipmapSignature.size()) == 0)
                            {
                                // Found — use the raw pixel data directly (RGBA, no LZ4 wrapper)
                                const size_t pixelBytes = static_cast<size_t>(result.width) * static_cast<size_t>(result.height) * 4u;
                                const size_t avail = dSize - (dp + mipmapSignature.size() - decomp.data());
                                if (avail >= pixelBytes)
                                {
                                    result.pixels.resize(pixelBytes);
                                    std::memcpy(result.pixels.data(), dp + mipmapSignature.size(), pixelBytes);
                                    return result;
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
        return fail();
    }

    const uint8_t* mipmapCursor = it + mipmapSignature.size();
    if (mipmapCursor + sizeof(uint32_t) * 3u > texSpan.data + texSpan.size)
        return fail();

    const uint32_t isLz4 = ReadLeU32(mipmapCursor);
    mipmapCursor += sizeof(uint32_t);
    const uint32_t decompressedSize = ReadLeU32(mipmapCursor);
    mipmapCursor += sizeof(uint32_t);
    const uint32_t compressedSize = ReadLeU32(mipmapCursor);
    mipmapCursor += sizeof(uint32_t);

    if (static_cast<size_t>(compressedSize) > static_cast<size_t>(texSpan.size - (mipmapCursor - texSpan.data)))
    {
        return fail();
    }

    std::vector<uint8_t> decodedPayload;
    const uint8_t* payloadPtr = nullptr;
    size_t payloadSize = 0;
    if (isLz4 == 1)
    {
        decodedPayload.resize(decompressedSize);
        const int decompressedResult = LZ4_decompress_safe(reinterpret_cast<const char*>(mipmapCursor),
                                                           reinterpret_cast<char*>(decodedPayload.data()), compressedSize,
                                                           decompressedSize);
        if (decompressedResult != static_cast<int>(decompressedSize))
        {
            return fail();
        }
        payloadPtr = decodedPayload.data();
        payloadSize = decodedPayload.size();
    }
    else
    {
        payloadPtr = mipmapCursor;
        payloadSize = compressedSize;
    }

    if (!payloadPtr)
        return fail();

    const size_t pixelCount = static_cast<size_t>(result.width) * static_cast<size_t>(result.height);
    result.pixels.resize(pixelCount * 4u);

    const auto requirePayload = [&](size_t bytesNeeded) -> bool
    {
        return payloadSize >= bytesNeeded;
    };

    const auto ceilDiv4 = [](int v) -> int
    {
        return (v + 3) / 4;
    };

    if (header->format == 0)
    {
        const size_t bytesNeeded = pixelCount * 4u;
        if (!requirePayload(bytesNeeded))
            return fail();

        std::memcpy(result.pixels.data(), payloadPtr, bytesNeeded);
        return result;
    }

    if (header->format == 8)
    {
        const size_t bytesNeeded = pixelCount * 2u;
        if (!requirePayload(bytesNeeded))
            return fail();

        for (size_t i = 0; i < pixelCount; ++i)
        {
            const uint8_t gray = payloadPtr[i * 2u + 0u];
            const uint8_t alpha = payloadPtr[i * 2u + 1u];
            uint8_t* dst = result.pixels.data() + i * 4u;
            dst[0] = gray;
            dst[1] = gray;
            dst[2] = gray;
            dst[3] = alpha;
        }
        return result;
    }

    if (header->format == 9)
    {
        const size_t bytesNeeded = pixelCount;
        if (!requirePayload(bytesNeeded))
            return fail();

        for (size_t i = 0; i < pixelCount; ++i)
        {
            const uint8_t alpha = payloadPtr[i];
            uint8_t* dst = result.pixels.data() + i * 4u;
            dst[0] = 255;
            dst[1] = 255;
            dst[2] = 255;
            dst[3] = alpha;
        }
        return result;
    }

    using BlockDecoder = void (*)(const void*, void*, int);
    BlockDecoder decoder = nullptr;
    size_t blockSize = 0;
    if (header->format == 4)
    {
        decoder = bcdec_bc3;
        blockSize = BCDEC_BC3_BLOCK_SIZE;
    }
    else if (header->format == 6)
    {
        decoder = bcdec_bc2;
        blockSize = BCDEC_BC2_BLOCK_SIZE;
    }
    else if (header->format == 7)
    {
        decoder = bcdec_bc1;
        blockSize = BCDEC_BC1_BLOCK_SIZE;
    }

    if (!decoder)
        return fail();

    const int xBlocks = ceilDiv4(result.width);
    const int yBlocks = ceilDiv4(result.height);
    const size_t bytesNeeded = static_cast<size_t>(xBlocks) * static_cast<size_t>(yBlocks) * blockSize;
    if (!requirePayload(bytesNeeded))
        return fail();

    const int destStride = result.width << 2;
    const uint8_t* srcBlock = payloadPtr;
    std::array<uint8_t, 4 * 4 * 4> edgeBlock{};

    for (int by = 0; by < yBlocks; ++by)
    {
        const int dstY = by * 4;
        const int copyH = (std::min)(4, result.height - dstY);
        for (int bx = 0; bx < xBlocks; ++bx)
        {
            const int dstX = bx * 4;
            const int copyW = (std::min)(4, result.width - dstX);
            uint8_t* dst = result.pixels.data() + (static_cast<size_t>(dstY) * result.width + dstX) * 4u;

            if (copyW == 4 && copyH == 4)
            {
                decoder(srcBlock, dst, destStride);
            }
            else
            {
                decoder(srcBlock, edgeBlock.data(), 4 * 4);
                for (int row = 0; row < copyH; ++row)
                {
                    std::memcpy(dst + static_cast<size_t>(row) * destStride, edgeBlock.data() + row * 4 * 4,
                                static_cast<size_t>(copyW) * 4u);
                }
            }
            srcBlock += blockSize;
        }
    }

    return result;
}

PkgParser::ThemeColor PkgParser::CalculateThemeColor(const RgbaImage& image) const
{
    ThemeColor result;
    if (!image.IsValid() || image.width <= 0 || image.height <= 0)
        return result;

    struct Bucket
    {
        long long sumR = 0;
        long long sumG = 0;
        long long sumB = 0;
        int count = 0;
    };

    static thread_local std::vector<Bucket> colorBuckets(1u << 15);
    std::fill(colorBuckets.begin(), colorBuckets.end(), Bucket{});

    constexpr int step = 8;
    constexpr int alphaThreshold = 128;
    constexpr int blackThreshold = 35;
    constexpr int whiteThreshold = 230;
    constexpr int saturationThreshold = 25;
    const int pixelStride = 4;
    const int rowStride = image.width << 2;
    const int sampleStride = step << 2;
    const int rowSampleStride = rowStride * step;

    int maxCount = 0;
    int bestBucketIdx = -1;
    long long fallbackR = 0;
    long long fallbackG = 0;
    long long fallbackB = 0;
    int fallbackCount = 0;

    for (int y = 0; y < image.height; y += step)
    {
        const uint8_t* row = image.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(rowStride);
        for (int x = 0; x < image.width; x += step)
        {
            const uint8_t* px = row + static_cast<size_t>(x << 2);
            const int r = px[0];
            const int g = px[1];
            const int b = px[2];
            const int a = px[3];
            (void)pixelStride;
            (void)sampleStride;
            (void)rowSampleStride;

            if (a < alphaThreshold)
                continue;

            fallbackR += r;
            fallbackG += g;
            fallbackB += b;
            ++fallbackCount;

            const int maxVal = (std::max)(r, (std::max)(g, b));
            const int minVal = (std::min)(r, (std::min)(g, b));
            const int saturation = maxVal - minVal;
            if (maxVal < blackThreshold || minVal > whiteThreshold || saturation < saturationThreshold)
                continue;

            const int bucketIdx = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
            Bucket& bucket = colorBuckets[static_cast<size_t>(bucketIdx)];
            bucket.sumR += r;
            bucket.sumG += g;
            bucket.sumB += b;
            ++bucket.count;
            if (bucket.count > maxCount)
            {
                maxCount = bucket.count;
                bestBucketIdx = bucketIdx;
            }
        }
    }

    if (bestBucketIdx >= 0)
    {
        const Bucket& best = colorBuckets[static_cast<size_t>(bestBucketIdx)];
        result.r = static_cast<int>(best.sumR / best.count);
        result.g = static_cast<int>(best.sumG / best.count);
        result.b = static_cast<int>(best.sumB / best.count);
        result.isValid = true;
    }
    else if (fallbackCount > 0)
    {
        result.r = static_cast<int>(fallbackR / fallbackCount);
        result.g = static_cast<int>(fallbackG / fallbackCount);
        result.b = static_cast<int>(fallbackB / fallbackCount);
        result.isValid = true;
    }

    return result;
}

bool PkgParser::CalcStatsFromRgba(const RgbaImage& img, double wPct, double hPct, double& outRoiAvg,
                                  double& outRoiDark, double& outGlobalAvg, double& outGlobalDark,
                                  double* outSurroundAvg) const
{
    if (!img.IsValid() || img.width <= 0 || img.height <= 0)
        return false;

    static double lut[256];
    static bool init = false;
    if (!init)
    {
        for (int i = 0; i < 256; ++i)
        {
            const double c = i / 255.0;
            lut[i] = (c <= 0.04045) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
        }
        init = true;
    }

    const int roiW = (std::max)(1, static_cast<int>(img.width * wPct));
    const int roiH = (std::max)(1, static_cast<int>(img.height * hPct));
    const int roiX = img.width - roiW;
    const int roiY = img.height - roiH;
    constexpr int step = 4;
    const int rowStride = img.width << 2;

    double sumGlobalL = 0.0;
    double sumRoiL = 0.0;
    int darkGlobal = 0;
    int darkRoi = 0;
    int globalCount = 0;
    int roiCount = 0;

    for (int y = 0; y < img.height; y += step)
    {
        const uint8_t* row = img.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(rowStride);
        for (int x = 0; x < img.width; x += step)
        {
            const uint8_t* px = row + static_cast<size_t>(x << 2);
            const uint8_t r = px[0];
            const uint8_t g = px[1];
            const uint8_t b = px[2];
            const uint8_t a = px[3];
            if (a < 10 || (r < 5 && g < 5 && b < 5))
                continue;

            const double L = 0.2126 * lut[r] + 0.7152 * lut[g] + 0.0722 * lut[b];
            sumGlobalL += L;
            ++globalCount;
            if (L < 0.179)
                ++darkGlobal;

            if (x >= roiX && y >= roiY)
            {
                sumRoiL += L;
                ++roiCount;
                if (L < 0.179)
                    ++darkRoi;
            }
        }
    }

    const int expectedSamples = (img.height >> 2) * (img.width >> 2);
    if (globalCount < static_cast<int>(expectedSamples * 0.4))
        return false;

    outGlobalAvg = (globalCount > 0) ? (sumGlobalL / globalCount) : 0.0;
    outGlobalDark = (globalCount > 0) ? (static_cast<double>(darkGlobal) / globalCount) : 0.0;
    if (roiCount > 0)
    {
        outRoiAvg = sumRoiL / roiCount;
        outRoiDark = static_cast<double>(darkRoi) / roiCount;
    }
    else
    {
        outRoiAvg = outGlobalAvg;
        outRoiDark = outGlobalDark;
    }

    if (outSurroundAvg)
    {
        const int surroundY0 = (std::max)(0, roiY - roiH);
        double sumSurround = 0.0;
        int surroundCount = 0;
        for (int y = surroundY0; y < roiY; y += step)
        {
            const uint8_t* row = img.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(rowStride);
            for (int x = roiX; x < img.width; x += step)
            {
                const uint8_t* px = row + static_cast<size_t>(x << 2);
                const uint8_t r = px[0], g = px[1], b = px[2];
                const uint8_t a = px[3];
                if (a < 10 || (r < 5 && g < 5 && b < 5))
                    continue;
                sumSurround += 0.2126 * lut[r] + 0.7152 * lut[g] + 0.0722 * lut[b];
                ++surroundCount;
            }
        }
        *outSurroundAvg = (surroundCount > 0) ? (sumSurround / surroundCount) : outRoiAvg;
    }

    return true;
}

double PkgParser::CalcEdgeDensity(const RgbaImage& image)
{
    if (!image.IsValid() || image.width < 8 || image.height < 8)
        return -1.0;

    static double lut[256];
    static bool init = false;
    if (!init)
    {
        for (int i = 0; i < 256; ++i)
        {
            const double c = i / 255.0;
            lut[i] = (c <= 0.04045) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
        }
        init = true;
    }

    constexpr int step = 4;
    constexpr double edgeThreshold = 0.12;
    const int rowStride = image.width << 2;

    int edgePixels = 0;
    int totalSamples = 0;

    for (int y = 0; y < image.height - step; y += step)
    {
        const uint8_t* row = image.pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(rowStride);
        const uint8_t* rowNext = image.pixels.data() + static_cast<size_t>(y + step) * static_cast<size_t>(rowStride);
        for (int x = 0; x < image.width - step; x += step)
        {
            const uint8_t* px = row + static_cast<size_t>(x << 2);
            const uint8_t* pxR = row + static_cast<size_t>(x + step) * 4u;
            const uint8_t* pxD = rowNext + static_cast<size_t>(x << 2);

            if (px[3] < 64 || pxR[3] < 64 || pxD[3] < 64)
                continue;

            double Lc = 0.2126 * lut[px[0]] + 0.7152 * lut[px[1]] + 0.0722 * lut[px[2]];
            double Lr = 0.2126 * lut[pxR[0]] + 0.7152 * lut[pxR[1]] + 0.0722 * lut[pxR[2]];
            double Ld = 0.2126 * lut[pxD[0]] + 0.7152 * lut[pxD[1]] + 0.0722 * lut[pxD[2]];

            double grad = std::abs(Lc - Lr) + std::abs(Lc - Ld);
            if (grad > edgeThreshold)
                ++edgePixels;
            ++totalSamples;
        }
    }

    if (totalSamples < 100)
        return -1.0;
    return static_cast<double>(edgePixels) / static_cast<double>(totalSamples);
}
