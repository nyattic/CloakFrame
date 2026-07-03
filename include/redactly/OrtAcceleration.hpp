#pragma once

#include <onnxruntime_cxx_api.h>

namespace redactly
{
    enum class OrtAccelerator
    {
        None,
        CoreML,
        DirectML,
    };

    [[nodiscard]] const char *ortAcceleratorName(OrtAccelerator accelerator);

    OrtAccelerator applyOrtAcceleration(Ort::SessionOptions &options, bool enabled);
}
