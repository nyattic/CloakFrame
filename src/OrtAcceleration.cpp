#include "redactly/OrtAcceleration.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

namespace redactly
{
    namespace
    {
        bool providerAvailable(const char *providerName)
        {
            const auto providers = Ort::GetAvailableProviders();
            return std::ranges::find(providers, std::string(providerName)) != providers.end();
        }
    }

    const char *ortAcceleratorName(OrtAccelerator accelerator)
    {
        switch (accelerator)
        {
            case OrtAccelerator::CoreML:
                return "CoreML";
            case OrtAccelerator::DirectML:
                return "DirectML";
            case OrtAccelerator::None:
                break;
        }
        return "CPU";
    }

    OrtAccelerator applyOrtAcceleration(Ort::SessionOptions &options, bool enabled)
    {
        if (!enabled)
        {
            return OrtAccelerator::None;
        }

#if defined(__APPLE__)
        if (providerAvailable("CoreMLExecutionProvider"))
        {
            try
            {
                const std::unordered_map<std::string, std::string> coremlOptions = {
                    {"ModelFormat", "MLProgram"},
                    {"MLComputeUnits", "ALL"},
                };
                options.AppendExecutionProvider("CoreML", coremlOptions);
                return OrtAccelerator::CoreML;
            }
            catch (const Ort::Exception &error)
            {
                spdlog::warn("CoreML acceleration unavailable, falling back to CPU: {}", error.what());
            }
        }
        else
        {
            spdlog::info("CoreML execution provider not present in this ONNX Runtime build; using CPU.");
        }
#elif defined(_WIN32)
        if (providerAvailable("DmlExecutionProvider"))
        {
            try
            {
                options.DisableMemPattern();
                options.SetExecutionMode(ORT_SEQUENTIAL);
                options.AppendExecutionProvider("DML", std::unordered_map<std::string, std::string>{});
                return OrtAccelerator::DirectML;
            }
            catch (const Ort::Exception &error)
            {
                options.EnableMemPattern();
                spdlog::warn("DirectML acceleration unavailable, falling back to CPU: {}", error.what());
            }
        }
        else
        {
            spdlog::info("DirectML execution provider not present in this ONNX Runtime build; using CPU.");
        }
#endif
        return OrtAccelerator::None;
    }
}
