#!/usr/bin/env bash
set -euo pipefail

if ! command -v sha256sum >/dev/null 2>&1; then
    sha256sum() { shasum -a 256 "$@"; }
fi

assets="${RUNNER_TEMP:?}/cloakframe-test-assets"
mkdir -p "$assets"

fetch() {
    local url="$1" name="$2" sha256="$3"
    curl -fsSL --retry 3 "$url" -o "$assets/$name"
    echo "$sha256  $assets/$name" | sha256sum -c -
}

fetch "https://github.com/yakhyo/yolov5-face-onnx-inference/releases/download/weights/yolov5n_face.onnx" \
    yolov5n_face.onnx \
    eb244a06e36999db732b317c2b30fa113cd6cfc1a397eaf738f2d6f33c01f640
fetch "https://github.com/opencv/opencv_zoo/raw/47534e27c9851bb1128ccc0102f1145e27f23f98/models/face_detection_yunet/face_detection_yunet_2023mar.onnx" \
    face_detection_yunet_2023mar.onnx \
    8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4
fetch "https://github.com/ankandrew/open-image-models/releases/download/assets/yolo-v9-t-512-license-plates-end2end.onnx" \
    yolo-v9-t-512-license-plates-end2end.onnx \
    746fdd358ec110418775d7c9d8d07910d48b1a21471f92bf4421f6510d6daade
fetch "https://github.com/scikit-image/scikit-image/raw/v0.25.2/skimage/data/astronaut.png" \
    astronaut.png \
    88431cd9653ccd539741b555fb0a46b61558b301d4110412b5bc28b5e3ea6cb5

{
    echo "CLOAKFRAME_TEST_YOLO5FACE_MODEL=$assets/yolov5n_face.onnx"
    echo "CLOAKFRAME_TEST_YUNET_MODEL=$assets/face_detection_yunet_2023mar.onnx"
    echo "CLOAKFRAME_TEST_PLATE_MODEL=$assets/yolo-v9-t-512-license-plates-end2end.onnx"
    echo "CLOAKFRAME_TEST_FACE_IMAGE=$assets/astronaut.png"
} >> "${GITHUB_ENV:?}"
