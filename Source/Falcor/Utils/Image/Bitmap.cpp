/***************************************************************************
 # Copyright (c) 2015-24, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/
#include "Bitmap.h"
#include "Core/API/Texture.h"
#include "Utils/Logger.h"
#include "Utils/StringUtils.h"

#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <vector>

namespace Falcor
{
static void genWarning(const std::string& errMsg, const std::filesystem::path& path)
{
    logWarning("Error when loading image file from '{}': {}", path, errMsg);
}

static bool isConvertibleToRGBA32Float(ResourceFormat format)
{
    FormatType type = getFormatType(format);
    bool isHalfFormat = (type == FormatType::Float && getNumChannelBits(format, 0) == 16);
    bool isLargeIntFormat = ((type == FormatType::Uint || type == FormatType::Sint) && getNumChannelBits(format, 0) >= 16);
    return isHalfFormat || isLargeIntFormat;
}

/**
 * Converts half float image to RGBA float image.
 */
static std::vector<float> convertHalfToRGBA32Float(uint32_t width, uint32_t height, uint32_t channelCount, const void* pData)
{
    std::vector<float> newData(width * height * 4u, 0.f);
    const float16_t* pSrc = reinterpret_cast<const float16_t*>(pData);
    float* pDst = newData.data();

    for (uint32_t i = 0; i < width * height; ++i)
    {
        for (uint32_t c = 0; c < channelCount; ++c)
        {
            *pDst++ = float(*pSrc++);
        }
        pDst += (4 - channelCount);
    }

    return newData;
}

/**
 * Converts integer image to RGBA float image.
 * Unsigned integers are normalized to [0,1], signed integers to [-1,1].
 */
template<typename SrcT>
static std::vector<float> convertIntToRGBA32Float(uint32_t width, uint32_t height, uint32_t channelCount, const void* pData)
{
    std::vector<float> newData(width * height * 4u, 0.f);
    const SrcT* pSrc = reinterpret_cast<const SrcT*>(pData);
    float* pDst = newData.data();

    for (uint32_t i = 0; i < width * height; ++i)
    {
        for (uint32_t c = 0; c < channelCount; ++c)
        {
            *pDst++ = float(*pSrc++) / float(std::numeric_limits<SrcT>::max());
        }
        pDst += (4 - channelCount);
    }

    return newData;
}

/**
 * Converts an image of the given format to an RGBA float image.
 */
static std::vector<float> convertToRGBA32Float(ResourceFormat format, uint32_t width, uint32_t height, const void* pData)
{
    FALCOR_ASSERT(isConvertibleToRGBA32Float(format));

    FormatType type = getFormatType(format);
    uint32_t channelCount = getFormatChannelCount(format);
    uint32_t channelBits = getNumChannelBits(format, 0);

    std::vector<float> floatData;

    if (type == FormatType::Float && channelBits == 16)
    {
        floatData = convertHalfToRGBA32Float(width, height, channelCount, pData);
    }
    else if (type == FormatType::Uint && channelBits == 16)
    {
        floatData = convertIntToRGBA32Float<uint16_t>(width, height, channelCount, pData);
    }
    else if (type == FormatType::Uint && channelBits == 32)
    {
        floatData = convertIntToRGBA32Float<uint32_t>(width, height, channelCount, pData);
    }
    else if (type == FormatType::Sint && channelBits == 16)
    {
        floatData = convertIntToRGBA32Float<int16_t>(width, height, channelCount, pData);
    }
    else if (type == FormatType::Sint && channelBits == 32)
    {
        floatData = convertIntToRGBA32Float<int32_t>(width, height, channelCount, pData);
    }
    else
    {
        FALCOR_UNREACHABLE();
    }

    // Default alpha channel to 1.
    if (channelCount < 4)
    {
        for (uint32_t i = 0; i < width * height; ++i)
            floatData[i * 4 + 3] = 1.f;
    }

    return floatData;
}

Bitmap::UniqueConstPtr Bitmap::create(uint32_t width, uint32_t height, ResourceFormat format, const uint8_t* pData)
{
    return Bitmap::UniqueConstPtr(new Bitmap(width, height, format, pData));
}

Bitmap::UniqueConstPtr Bitmap::createFromFile(const std::filesystem::path& path, bool isTopDown, ImportFlags importFlags)
{
    if (!std::filesystem::exists(path))
    {
        logWarning("Error when loading image file. File '{}' does not exist.", path);
        return nullptr;
    }

    auto input = OIIO::ImageInput::open(path.string());
    if (!input)
    {
        genWarning(OIIO::geterror(), path);
        return nullptr;
    }

    const OIIO::ImageSpec& spec = input->spec();
    const uint32_t width = uint32_t(spec.width);
    const uint32_t height = uint32_t(spec.height);
    const uint32_t srcChannels = uint32_t(spec.nchannels);

    if (width == 0 || height == 0 || srcChannels == 0)
    {
        genWarning("Invalid image", path);
        return nullptr;
    }

    // Identify the resource format and the per-channel type we read from OpenImageIO.
    // We always expand color images to 4 channels (RGBA) since RGB-only formats are not generally supported.
    const OIIO::TypeDesc::BASETYPE baseType = OIIO::TypeDesc::BASETYPE(spec.format.basetype);
    const bool isFloatType = (baseType == OIIO::TypeDesc::FLOAT || baseType == OIIO::TypeDesc::DOUBLE || baseType == OIIO::TypeDesc::HALF);
    const bool is16BitInt = (baseType == OIIO::TypeDesc::UINT16 || baseType == OIIO::TypeDesc::INT16);

    ResourceFormat format = ResourceFormat::Unknown;
    OIIO::TypeDesc readType;
    uint32_t dstChannels = 4;

    if (isFloatType)
    {
        if (baseType == OIIO::TypeDesc::HALF || is_set(importFlags, ImportFlags::ConvertToFloat16))
        {
            format = ResourceFormat::RGBA16Float;
            readType = OIIO::TypeDesc::HALF;
        }
        else
        {
            format = ResourceFormat::RGBA32Float;
            readType = OIIO::TypeDesc::FLOAT;
        }
        dstChannels = 4;
    }
    else if (is16BitInt)
    {
        readType = OIIO::TypeDesc::UINT16;
        if (srcChannels == 1)
        {
            format = ResourceFormat::R16Unorm;
            dstChannels = 1;
        }
        else
        {
            format = ResourceFormat::RGBA16Unorm;
            dstChannels = 4;
        }
    }
    else
    {
        readType = OIIO::TypeDesc::UINT8;
        if (srcChannels == 1)
        {
            format = ResourceFormat::R8Unorm;
            dstChannels = 1;
        }
        else if (srcChannels == 2)
        {
            format = ResourceFormat::RG8Unorm;
            dstChannels = 2;
        }
        else
        {
            format = ResourceFormat::RGBA8Unorm;
            dstChannels = 4;
        }
    }

    // Read the native channels (converted to readType) into a temporary buffer.
    const size_t elemSize = readType.size();
    std::vector<uint8_t> src(size_t(width) * height * srcChannels * elemSize);
    if (!input->read_image(0, 0, 0, srcChannels, readType, src.data()))
    {
        genWarning(input->geterror(), path);
        return nullptr;
    }
    input->close();

    // Fill value for a synthesized alpha channel (opaque).
    uint8_t alphaFill[sizeof(float)] = {};
    switch (readType.basetype)
    {
    case OIIO::TypeDesc::FLOAT:
    {
        const float one = 1.f;
        std::memcpy(alphaFill, &one, sizeof(one));
        break;
    }
    case OIIO::TypeDesc::HALF:
    {
        const uint16_t one = float16_t(1.f).toBits();
        std::memcpy(alphaFill, &one, sizeof(one));
        break;
    }
    case OIIO::TypeDesc::UINT16:
    {
        const uint16_t one = 0xffff;
        std::memcpy(alphaFill, &one, sizeof(one));
        break;
    }
    default: // UINT8
        alphaFill[0] = 0xff;
        break;
    }

    // OpenImageIO returns pixels top-down. Flip rows if the caller requested bottom-up layout.
    UniqueConstPtr pBmp = UniqueConstPtr(new Bitmap(width, height, format));
    uint8_t* dst = pBmp->getData();
    const size_t srcRowSize = size_t(width) * srcChannels * elemSize;
    const size_t dstRowSize = size_t(width) * dstChannels * elemSize;

    for (uint32_t y = 0; y < height; ++y)
    {
        const uint8_t* srcRow = src.data() + size_t(y) * srcRowSize;
        uint8_t* dstRow = dst + size_t(isTopDown ? y : (height - 1 - y)) * dstRowSize;

        for (uint32_t x = 0; x < width; ++x)
        {
            const uint8_t* srcPixel = srcRow + size_t(x) * srcChannels * elemSize;
            uint8_t* dstPixel = dstRow + size_t(x) * dstChannels * elemSize;

            for (uint32_t c = 0; c < dstChannels; ++c)
            {
                if (c < srcChannels)
                    std::memcpy(dstPixel + c * elemSize, srcPixel + c * elemSize, elemSize);
                else if (c == dstChannels - 1)
                    std::memcpy(dstPixel + c * elemSize, alphaFill, elemSize);
                else
                    std::memset(dstPixel + c * elemSize, 0, elemSize);
            }
        }
    }

    return pBmp;
}

Bitmap::Bitmap(uint32_t width, uint32_t height, ResourceFormat format)
    : mWidth(width), mHeight(height), mRowPitch(getFormatRowPitch(format, width)), mFormat(format)
{
    if (isCompressedFormat(format))
    {
        uint32_t blockSizeY = getFormatHeightCompressionRatio(format);
        FALCOR_ASSERT(height % blockSizeY == 0); // Should divide evenly
        mSize = size_t(mRowPitch) * (height / blockSizeY);
    }
    else
    {
        mSize = height * size_t(mRowPitch);
    }

    mpData = std::unique_ptr<uint8_t[]>(new uint8_t[mSize]);
}

Bitmap::Bitmap(uint32_t width, uint32_t height, ResourceFormat format, const uint8_t* pData) : Bitmap(width, height, format)
{
    std::memcpy(mpData.get(), pData, mSize);
}

Bitmap::FileFormat Bitmap::getFormatFromFileExtension(const std::string& ext)
{
    // This array is in the order of the enum
    static const char* kExtensions[] = {
        /* PngFile */ "png",
        /*JpegFile */ "jpg",
        /* TgaFile */ "tga",
        /* BmpFile */ "bmp",
        /* PfmFile */ "pfm",
        /* ExrFile */ "exr",
        /* DdsFile */ "dds",
    };

    for (size_t i = 0; i < std::size(kExtensions); i++)
    {
        if (kExtensions[i] == ext)
            return Bitmap::FileFormat(i);
    }
    FALCOR_THROW("Can't find a matching format for file extension '{}'.", ext);
}

FileDialogFilterVec Bitmap::getFileDialogFilters(ResourceFormat format)
{
    FileDialogFilterVec filters;
    bool showHdr = true;
    bool showLdr = true;

    if (format != ResourceFormat::Unknown)
    {
        // Save float, half and large integer (16/32 bit) formats as HDR.
        showHdr = getFormatType(format) == FormatType::Float || isConvertibleToRGBA32Float(format);
        showLdr = !showHdr;
    }

    if (showHdr)
    {
        filters.push_back({"exr", "High Dynamic Range"});
        filters.push_back({"pfm", "Portable Float Map"});
        filters.push_back({"hdr", "Radiance HDR"});
    }

    if (showLdr)
    {
        filters.push_back({"png", "Portable Network Graphics"});
        filters.push_back({"jpg", "JPEG"});
        filters.push_back({"bmp", "Bitmap Image File"});
        filters.push_back({"tga", "Truevision Graphics Adapter"});
    }

    // DDS can store all formats
    filters.push_back({"dds", "DirectDraw Surface"});

    // List of formats we can only load from
    if (format == ResourceFormat::Unknown)
    {
        filters.push_back({"hdr", "High Dynamic Range"});
    }
    return filters;
}

std::string Bitmap::getFileExtFromResourceFormat(ResourceFormat format)
{
    auto filters = getFileDialogFilters(format);
    return filters.front().ext;
}

void Bitmap::saveImageDialog(Texture* pTexture)
{
    std::filesystem::path path;
    auto supportExtensions = getFileDialogFilters(pTexture->getFormat());

    if (saveFileDialog(supportExtensions, path))
    {
        std::string ext = getExtensionFromPath(path);
        auto format = getFormatFromFileExtension(ext);
        pTexture->captureToFile(0, 0, path, format);
    }
}

static bool isBGRFormat(ResourceFormat format)
{
    return format == ResourceFormat::BGRA8Unorm || format == ResourceFormat::BGRA8UnormSrgb ||
           format == ResourceFormat::BGRX8Unorm || format == ResourceFormat::BGRX8UnormSrgb;
}

void Bitmap::saveImage(
    const std::filesystem::path& path,
    uint32_t width,
    uint32_t height,
    FileFormat fileFormat,
    ExportFlags exportFlags,
    ResourceFormat resourceFormat,
    bool isTopDown,
    void* pData
)
{
    FALCOR_CHECK(pData, "Provided data must not be nullptr.");
    FALCOR_CHECK(fileFormat != FileFormat::DdsFile, "Cannot save DDS files. Use ImageIO instead.");
    if (is_set(exportFlags, ExportFlags::Uncompressed) && is_set(exportFlags, ExportFlags::Lossy))
        FALCOR_THROW("Incompatible flags: lossy cannot be combined with uncompressed.");
    if (is_set(exportFlags, ExportFlags::ExrFloat16) &&
        (!is_set(exportFlags, ExportFlags::Uncompressed) || fileFormat != FileFormat::ExrFile))
        FALCOR_THROW("Incompatible flags: EXR float16 can only be set for uncompressed EXR files.");

    const bool exportAlpha = is_set(exportFlags, ExportFlags::ExportAlpha);
    const bool isHdr = (fileFormat == FileFormat::PfmFile || fileFormat == FileFormat::ExrFile);

    OIIO::TypeDesc sourceType; // Type of the data we hand to OpenImageIO.
    OIIO::TypeDesc fileType;   // Per-channel type stored in the file.
    uint32_t srcChannels = 0;  // Channels present in the source buffer (stride between pixels).
    uint32_t outChannels = 0;  // Channels written to the file.

    // Source pointer (may be redirected to a converted/swizzled scratch buffer below).
    const uint8_t* src = reinterpret_cast<const uint8_t*>(pData);
    std::vector<float> floatScratch;
    std::vector<uint8_t> byteScratch;
    std::vector<std::string> warnings;

    OIIO::ImageSpec spec;

    if (isHdr)
    {
        sourceType = OIIO::TypeDesc::FLOAT;

        if (isConvertibleToRGBA32Float(resourceFormat))
        {
            floatScratch = convertToRGBA32Float(resourceFormat, width, height, pData);
            src = reinterpret_cast<const uint8_t*>(floatScratch.data());
            srcChannels = 4;
        }
        else if (getFormatType(resourceFormat) == FormatType::Float && getNumChannelBits(resourceFormat, 0) == 32)
        {
            srcChannels = getFormatChannelCount(resourceFormat);
        }
        else
        {
            FALCOR_THROW("Only support for floating-point or convertible formats as PFM/EXR files.");
        }

        if (fileFormat == FileFormat::PfmFile)
        {
            FALCOR_CHECK(!is_set(exportFlags, ExportFlags::Lossy), "PFM does not support lossy compression mode.");
            FALCOR_CHECK(!exportAlpha, "PFM does not support alpha channel.");
            outChannels = std::min(srcChannels, 3u);
            fileType = OIIO::TypeDesc::FLOAT;
        }
        else // ExrFile
        {
            // Write all available channels, dropping alpha from RGBA sources unless explicitly requested.
            outChannels = (exportAlpha && srcChannels == 4) ? 4 : std::min(srcChannels, 3u);
            fileType = is_set(exportFlags, ExportFlags::ExrFloat16) ? OIIO::TypeDesc::HALF : OIIO::TypeDesc::FLOAT;
        }

        spec = OIIO::ImageSpec(int(width), int(height), int(outChannels), fileType);

        if (fileFormat == FileFormat::ExrFile)
        {
            if (is_set(exportFlags, ExportFlags::Uncompressed))
                spec.attribute("compression", "none");
            else if (is_set(exportFlags, ExportFlags::Lossy))
                spec.attribute("compression", "b44");
            else
                spec.attribute("compression", "zip");
        }
    }
    else
    {
        sourceType = OIIO::TypeDesc::UINT8;
        fileType = OIIO::TypeDesc::UINT8;
        srcChannels = getFormatChannelCount(resourceFormat);

        // OpenImageIO writes channels in RGBA order. Swizzle BGRA/BGRX sources to RGBA.
        if (isBGRFormat(resourceFormat) && srcChannels == 4)
        {
            byteScratch.assign(reinterpret_cast<const uint8_t*>(pData),
                               reinterpret_cast<const uint8_t*>(pData) + size_t(width) * height * 4);
            for (size_t i = 0; i < size_t(width) * height; ++i)
                std::swap(byteScratch[i * 4 + 0], byteScratch[i * 4 + 2]);
            src = byteScratch.data();
        }

        const bool fmtSupportsAlpha = (fileFormat == FileFormat::PngFile || fileFormat == FileFormat::TgaFile);
        outChannels = (exportAlpha && srcChannels == 4 && fmtSupportsAlpha) ? 4 : std::min(srcChannels, 3u);

        spec = OIIO::ImageSpec(int(width), int(height), int(outChannels), fileType);

        switch (fileFormat)
        {
        case FileFormat::JpegFile:
            spec.attribute("CompressionQuality", is_set(exportFlags, ExportFlags::Lossy) ? 90 : 100);
            spec.attribute("jpeg:subsampling", "4:4:4");
            if (exportAlpha)
                warnings.push_back("JPEG format does not support alpha channel.");
            break;
        case FileFormat::PngFile:
            spec.attribute("png:compressionLevel", is_set(exportFlags, ExportFlags::Uncompressed) ? 0 : 9);
            if (is_set(exportFlags, ExportFlags::Lossy))
                warnings.push_back("PNG format does not support lossy compression mode.");
            break;
        case FileFormat::TgaFile:
            spec.attribute("compression", "none");
            if (is_set(exportFlags, ExportFlags::Lossy))
                warnings.push_back("TGA format does not support lossy compression mode.");
            break;
        case FileFormat::BmpFile:
            spec.attribute("compression", "none");
            if (is_set(exportFlags, ExportFlags::Lossy))
                warnings.push_back("BMP format does not support lossy compression mode.");
            if (exportAlpha)
                warnings.push_back("BMP format does not support alpha channel.");
            break;
        default:
            FALCOR_UNREACHABLE();
        }
    }

    if (!warnings.empty())
        logWarning("Bitmap::saveImage: {}", joinStrings(warnings, " "));

    auto output = OIIO::ImageOutput::create(path.string());
    if (!output)
        FALCOR_THROW("Failed to create image writer for '{}': {}", path, OIIO::geterror());

    if (!output->open(path.string(), spec))
        FALCOR_THROW("Failed to open '{}' for writing: {}", path, output->geterror());

    // Stride between pixels in the source buffer (keeps extra channels that are not written).
    const OIIO::stride_t xstride = OIIO::stride_t(srcChannels) * sourceType.size();
    OIIO::stride_t ystride = xstride * width;
    const uint8_t* base = src;
    if (!isTopDown)
    {
        base += size_t(ystride) * (height - 1);
        ystride = -ystride;
    }

    if (!output->write_image(sourceType, base, xstride, ystride))
    {
        const std::string error = output->geterror();
        output->close();
        FALCOR_THROW("Failed to write image '{}': {}", path, error);
    }

    output->close();
}
} // namespace Falcor
