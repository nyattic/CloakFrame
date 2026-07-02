#pragma once

#include "faceveil/FaceDetection.hpp"

#include <opencv2/core.hpp>

namespace faceveil
{
    class Detector
    {
    public:
        virtual ~Detector() = default;

        virtual FaceDetections detect(const cv::Mat &bgrImage, float scoreThreshold, float nmsThreshold) = 0;
    };
}
