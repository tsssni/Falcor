/***************************************************************************
 # Copyright (c) 2015-23, NVIDIA CORPORATION. All rights reserved.
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
#include <OpenImageIO/imageio.h>
#include <args.hxx>

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <stdexcept>
#include <map>
#include <functional>
#include <filesystem>

#include <cmath>
#include <cstring>

template<typename T>
T sqr(T x)
{
    return x * x;
}

template<typename T>
T lerp(T a, T b, T t)
{
    return a + t * (b - a);
}

template<typename T>
T clamp(T x, T lo, T hi)
{
    return std::max(lo, std::min(hi, x));
}

class Image
{
public:
    Image(uint32_t width, uint32_t height) : mWidth(width), mHeight(height), mData(std::make_unique<float[]>(width * height * 4)) {}

    uint32_t getWidth() const { return mWidth; }
    uint32_t getHeight() const { return mHeight; }
    const float* getData() const { return mData.get(); }
    float* getData() { return mData.get(); }

    static std::shared_ptr<Image> create(uint32_t width, uint32_t height) { return std::make_shared<Image>(width, height); }

    static std::shared_ptr<Image> loadFromFile(const std::filesystem::path& path)
    {
        auto input = OIIO::ImageInput::open(path.string());
        if (!input)
            throw std::runtime_error("Cannot read image");

        const OIIO::ImageSpec& spec = input->spec();
        const uint32_t width = uint32_t(spec.width);
        const uint32_t height = uint32_t(spec.height);
        const int channels = spec.nchannels;

        // Read the source channels as float (top-down).
        std::vector<float> src(size_t(width) * height * channels);
        if (!input->read_image(0, 0, 0, channels, OIIO::TypeDesc::FLOAT, src.data()))
            throw std::runtime_error("Cannot read image");
        input->close();

        // Expand to RGBA, defaulting missing color channels to 0 and alpha to 1.
        auto image = create(width, height);
        float* dst = image->getData();
        for (size_t i = 0; i < size_t(width) * height; ++i)
        {
            for (int c = 0; c < 4; ++c)
                dst[i * 4 + c] = (c < channels) ? src[i * channels + c] : (c == 3 ? 1.f : 0.f);
        }

        return image;
    }

    void saveToFile(const std::filesystem::path& path, bool writeAlpha = true) const
    {
        auto output = OIIO::ImageOutput::create(path.string());
        if (!output)
            throw std::runtime_error("Unknown or unsupported image format");

        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        const bool writeFloat = (ext == ".exr" || ext == ".pfm" || ext == ".hdr");
        if (ext != ".exr" && ext != ".png")
            writeAlpha = false;

        const int channels = writeAlpha ? 4 : 3;
        const OIIO::TypeDesc fileType = writeFloat ? OIIO::TypeDesc::FLOAT : OIIO::TypeDesc::UINT8;
        OIIO::ImageSpec spec(int(mWidth), int(mHeight), channels, fileType);

        // Source is tightly packed RGBA float, top-down. Writing 3 channels reads RGB from each
        // RGBA pixel via the pixel stride; float->uint8 conversion (with clamping) is done by OIIO.
        if (!output->open(path.string(), spec))
            throw std::runtime_error("Cannot open image for writing");
        const OIIO::stride_t xstride = OIIO::stride_t(4) * OIIO::stride_t(sizeof(float));
        if (!output->write_image(OIIO::TypeDesc::FLOAT, getData(), xstride))
            throw std::runtime_error("Cannot write image");
        output->close();
    }

private:
    uint32_t mWidth;
    uint32_t mHeight;
    std::unique_ptr<float[]> mData;
};

struct MSE
{
    double operator()(const float* a, const float* b, size_t count) const
    {
        double error = 0.0;
        for (size_t i = 0; i < count; ++i)
            error += sqr(a[i] - b[i]);
        return error / count;
    }
};

struct RMSE
{
    double operator()(const float* a, const float* b, size_t count) const
    {
        double error = 0.0;
        for (size_t i = 0; i < count; ++i)
            error += sqr(a[i] - b[i]) / (sqr(a[i]) + 1e-3);
        return error / count;
    }
};

struct MAE
{
    double operator()(const float* a, const float* b, size_t count) const
    {
        double error = 0.0;
        for (size_t i = 0; i < count; ++i)
            error += std::fabs(sqr(a[i] - b[i]));
        return error / count;
    }
};

struct MAPE
{
    double operator()(const float* a, const float* b, size_t count) const
    {
        double error = 0.0;
        for (size_t i = 0; i < count; ++i)
            error += std::fabs((a[i] - b[i]) / (a[i] + 1e-3));
        return 100.0 * error / count;
    }
};

template<typename Metric>
double compare(const Image& imageA, const Image& imageB, bool alpha, float* errorMap)
{
    Metric metric;
    double sum = 0.0;
    const float* a = imageA.getData();
    const float* b = imageB.getData();
    size_t count = imageA.getWidth() * imageA.getHeight();
    for (size_t i = 0; i < count; ++i)
    {
        double error = metric(a, b, alpha ? 4 : 3);
        if (errorMap)
            *errorMap++ = float(error);
        sum += error;
        a += 4;
        b += 4;
    }
    return sum / count;
}

struct ErrorMetric
{
    std::string name;
    std::string desc;
    std::function<double(const Image& imageA, const Image& imageB, bool alpha, float* errorMap)> compare;
};

static const std::vector<ErrorMetric> errorMetrics = {
    {"mse", "Mean Squared Error", compare<MSE>},
    {"rmse", "Relative Mean Squared Error", compare<RMSE>},
    {"mae", "Mean Absolute Error", compare<MAE>},
    {"mape", "Mean Absolute Percentage Error", compare<MAPE>},
};

static std::shared_ptr<Image> generateHeatMap(uint32_t width, uint32_t height, const float* errorMap)
{
    auto writeColor = [](float t, float* dst)
    {
        static const float colors[5][3] = {
            {0.f, 0.f, 1.f}, // blue
            {0.f, 1.f, 1.f}, // teal
            {0.f, 1.f, 0.f}, // green
            {1.f, 1.f, 0.f}, // yellow
            {1.f, 0.f, 0.f}, // red
        };

        int c = clamp(int(std::floor(t * 4.f)), 0, 3);
        for (size_t i = 0; i < 3; ++i)
            *dst++ = lerp(colors[c][i], colors[c + 1][i], t * 4.f - c);
        *dst++ = 1.f;
    };

    const auto [minValue, maxValue] = std::minmax_element(errorMap, errorMap + width * height);
    const float range = std::max(1e-5f, *maxValue - *minValue);
    auto image = Image::create(width, height);
    float* dst = image->getData();
    for (size_t i = 0; i < width * height; ++i)
    {
        float t = clamp((errorMap[i] - *minValue) / range, 0.f, 1.f);
        writeColor(t, dst);
        dst += 4;
    }

    return image;
}

static bool compareImages(
    const std::filesystem::path& pathA,
    const std::filesystem::path& pathB,
    ErrorMetric metric,
    float threshold,
    bool alpha,
    const std::filesystem::path& heatMapPath
)
{
    auto loadImage = [](const std::filesystem::path& path)
    {
        try
        {
            return Image::loadFromFile(path);
        }
        catch (const std::runtime_error& e)
        {
            std::cerr << "Cannot load image from '" << path.string() << "' (Error: " << e.what() << ")." << std::endl;
            return std::shared_ptr<Image>{};
        }
    };

    auto saveImage = [](const Image& image, const std::filesystem::path& path)
    {
        try
        {
            image.saveToFile(path);
        }
        catch (const std::runtime_error& e)
        {
            std::cerr << "Cannot save image to '" << path.string() << "' (Error: " << e.what() << ")." << std::endl;
        }
    };

    // Load images.
    auto imageA = loadImage(pathA);
    if (!imageA)
        return false;
    auto imageB = loadImage(pathB);
    if (!imageB)
        return false;

    // Check resolution.
    if (imageA->getWidth() != imageB->getWidth() || imageA->getHeight() != imageB->getHeight())
    {
        std::cerr << "Cannot compare images with different resolutions." << std::endl;
        return false;
    }

    uint32_t width = imageA->getWidth();
    uint32_t height = imageB->getHeight();

    // Compare images.
    std::unique_ptr<float[]> errorMap = heatMapPath.empty() ? nullptr : std::make_unique<float[]>(width * height);
    double error = metric.compare(*imageA, *imageB, alpha, errorMap.get());

    // Generate heat map.
    if (errorMap)
    {
        auto heatMap = generateHeatMap(width, height, errorMap.get());
        saveImage(*heatMap, heatMapPath);
    }

    std::cout << error << std::endl;

    // Treat nans and infs as errors.
    if (std::isnan(error) || std::isinf(error))
        return false;

    return error <= threshold;
}

static void printMetrics(std::ostream& stream = std::cout)
{
    stream << "Available error metrics:" << std::endl;
    for (const auto& metric : errorMetrics)
    {
        stream << "  " << metric.name << " - " << metric.desc << std::endl;
    }
}

int main(int argc, char** argv)
{
    args::ArgumentParser parser("Utility to compare images.");
    parser.helpParams.programName = "ImageCompare";
    args::HelpFlag helpFlag(parser, "help", "Display this help menu.", {'h', "help"});
    args::Flag listMetricsFlag(parser, "", "List available error metrics.", {'l'});
    args::ValueFlag<std::string> metricFlag(parser, "metric", "The error metric.", {'m'});
    args::ValueFlag<float> thresholdFlag(parser, "threshold", "The error threshold.", {'t'});
    args::Flag alphaFlag(parser, "", "Include alpha channel.", {'a'});
    args::ValueFlag<std::string> heatMapFlag(parser, "filename", "Generate error heat map.", {'e'});
    args::Positional<std::string> image1(parser, "image1", "The first image.", args::Options::Required);
    args::Positional<std::string> image2(parser, "image2", "The second image.", args::Options::Required);
    args::CompletionFlag completionFlag(parser, {"complete"});

    try
    {
        parser.ParseCLI(argc, argv);
    }
    catch (const args::Completion& e)
    {
        std::cout << e.what();
        return 0;
    }
    catch (const args::Help&)
    {
        std::cout << parser;
        return 0;
    }
    catch (const args::ParseError& e)
    {
        std::cerr << e.what() << std::endl;
        std::cerr << parser;
        return 1;
    }
    catch (const args::RequiredError& e)
    {
        std::cerr << e.what() << std::endl;
        std::cerr << parser;
        return 1;
    }

    if (listMetricsFlag)
    {
        printMetrics();
        return 0;
    }

    ErrorMetric metric = errorMetrics.front();
    if (metricFlag)
    {
        auto name = args::get(metricFlag);
        auto it =
            std::find_if(errorMetrics.begin(), errorMetrics.end(), [&name](const ErrorMetric& metric) { return metric.name == name; });
        if (it == errorMetrics.end())
        {
            std::cerr << "Unknown error metric '" << args::get(metricFlag) << "'." << std::endl;
            printMetrics(std::cerr);
            return 1;
        }
        metric = *it;
    }

    bool success = compareImages(
        args::get(image1),
        args::get(image2),
        metric,
        thresholdFlag ? args::get(thresholdFlag) : 0.f,
        alphaFlag ? args::get(alphaFlag) : false,
        heatMapFlag ? args::get(heatMapFlag) : ""
    );
    return success ? 0 : 1;
}
