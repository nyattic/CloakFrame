#include "redactly/DetectionGeometry.hpp"
#include "redactly/ImageIo.hpp"
#include "redactly/ImageScanner.hpp"
#include "redactly/Mosaic.hpp"
#include "redactly/ModelCatalog.hpp"
#include "redactly/OnnxGraphPatch.hpp"
#include "redactly/OrtAcceleration.hpp"
#include "redactly/OutputPlan.hpp"
#include "redactly/PathSafety.hpp"
#include "redactly/PlateDetector.hpp"
#include "redactly/ProcessorWorker.hpp"
#include "redactly/ReviewTypes.hpp"
#include "redactly/ScrfdFaceDetector.hpp"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>

#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#ifdef REDACTLY_HAVE_EXIV2
#include <exiv2/exiv2.hpp>
#endif

namespace
{
    class CopyOriginalReviewer final : public QObject
    {
        Q_OBJECT

    public slots:
        redactly::ReviewResult requestReview(const QImage &, const QString &,
                                             const QVector<QRectF> &, int, int, double)
        {
            redactly::ReviewResult result;
            result.decision = redactly::ReviewDecision::CopyOriginal;
            return result;
        }
    };

    class ReplacingCopyOriginalReviewer final : public QObject
    {
        Q_OBJECT

    public:
        ReplacingCopyOriginalReviewer(QString source, QString replacement)
            : source_(std::move(source)), replacement_(std::move(replacement))
        {
        }

    public slots:
        redactly::ReviewResult requestReview(const QImage &, const QString &,
                                             const QVector<QRectF> &, int, int, double)
        {
            assert(QFile::remove(source_));
            assert(QFile::rename(replacement_, source_));
            redactly::ReviewResult result;
            result.decision = redactly::ReviewDecision::CopyOriginal;
            return result;
        }

    private:
        QString source_;
        QString replacement_;
    };

    void testAccelerationBackendNames()
    {
        assert(std::string(redactly::ortAcceleratorName(redactly::OrtAccelerator::None)) == "CPU");
        assert(std::string(redactly::ortAcceleratorName(redactly::OrtAccelerator::CoreML)) == "CoreML");
        assert(std::string(redactly::ortAcceleratorName(redactly::OrtAccelerator::DirectML)) == "DirectML");
        assert(std::string(redactly::ortAcceleratorName(redactly::OrtAccelerator::CUDA)) == "CUDA");
        assert(std::string(redactly::ortAcceleratorName(redactly::OrtAccelerator::MIGraphX)) == "MIGraphX");
        assert(std::string(redactly::ortAcceleratorName(redactly::OrtAccelerator::ROCm)) == "ROCm");
    }

    void testBuiltinModelDigests()
    {
        for (const auto &model: redactly::builtinModels())
        {
            const QByteArray digest = QByteArray::fromHex(model.sha256.toLatin1());
            assert(redactly::modelDigestMatches(model, digest));
            QByteArray changed = digest;
            changed[0] = static_cast<char>(changed[0] ^ 0x01);
            assert(!redactly::modelDigestMatches(model, changed));
        }
        const auto &plate = redactly::plateModel();
        assert(redactly::modelDigestMatches(
            plate, QByteArray::fromHex(plate.sha256.toLatin1())));
    }

    void writeBytes(const QString &path)
    {
        QFile file(path);
        assert(file.open(QIODevice::WriteOnly));
        assert(file.write("x") == 1);
    }

    void writeBytes(const std::filesystem::path &path, const std::vector<uchar> &bytes)
    {
        QFile file(QString::fromStdString(path.string()));
        assert(file.open(QIODevice::WriteOnly));
        assert(file.write(reinterpret_cast<const char *>(bytes.data()),
                          static_cast<qint64>(bytes.size())) == static_cast<qint64>(bytes.size()));
    }

    void writeJpegWithExifOrientation(const std::filesystem::path &path,
                                      const unsigned char orientation)
    {
        std::vector<uchar> jpeg;
        cv::Mat pixels(2, 3, CV_8UC3, cv::Scalar(20, 40, 60));
        assert(cv::imencode(".jpg", pixels, jpeg));
        assert(jpeg.size() >= 2 && jpeg[0] == 0xFF && jpeg[1] == 0xD8);

        const std::vector<uchar> exif = {
            0xFF, 0xE1, 0x00, 0x22,
            'E', 'x', 'i', 'f', 0x00, 0x00,
            'I', 'I', 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00,
            0x01, 0x00,
            0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00,
            orientation, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
        };
        jpeg.insert(jpeg.begin() + 2, exif.begin(), exif.end());

        QFile file(QString::fromStdString(path.string()));
        assert(file.open(QIODevice::WriteOnly));
        assert(file.write(reinterpret_cast<const char *>(jpeg.data()),
                          static_cast<qint64>(jpeg.size())) == static_cast<qint64>(jpeg.size()));
    }

    void testSupportedImageExtensions()
    {
        assert(redactly::isSupportedImage("photo.jpg"));
        assert(redactly::isSupportedImage("photo.JPEG"));
        assert(redactly::isSupportedImage("photo.webp"));
        assert(!redactly::isSupportedImage("photo.txt"));
    }

    void testScanImagesRecursesAndDeduplicates()
    {
        QTemporaryDir temp;
        assert(temp.isValid());

        QDir root(temp.path());
        assert(root.mkpath("a/nested"));
        assert(root.mkpath("b"));

        const QString first = root.filePath("a/one.JPG");
        const QString second = root.filePath("a/nested/two.png");
        const QString ignored = root.filePath("b/notes.txt");
        writeBytes(first);
        writeBytes(second);
        writeBytes(ignored);

        const auto nonRecursive = redactly::scanImages({root.filePath("a")}, false);
        assert(nonRecursive.size() == 1);

        const auto recursive = redactly::scanImages({root.filePath("a"), first}, true);
        assert(recursive.size() == 2);

        std::set<std::string> relativePaths;
        for (const auto &item: recursive)
        {
            relativePaths.insert(item.relativePath.generic_string());
        }
        assert(relativePaths.contains("one.JPG"));
        assert(relativePaths.contains("nested/two.png"));
    }

    void testOutputPlanRejectsExistingAndDuplicateDestinations()
    {
        QTemporaryDir temp;
        assert(temp.isValid());
        const std::filesystem::path root(temp.path().toStdString());
        const auto output = root / "out";
        assert(std::filesystem::create_directories(output / "nested"));

        const std::vector<redactly::ScanResult> unique = {
            {root / "input" / "one.jpg", "one.jpg"},
            {root / "input" / "two.mov", "nested/two.mov"},
        };
        assert(redactly::findOutputConflicts(unique, output).empty());
        assert(redactly::outputRelativePath(unique[1]) == std::filesystem::path("nested/two.mp4"));

        writeBytes(QString::fromStdString((output / "one.jpg").string()));
        const auto existing = redactly::findOutputConflicts(unique, output);
        assert(existing.size() == 1);
        assert(existing[0].kind == redactly::OutputConflict::Kind::ExistingDestination);

        const std::vector<redactly::ScanResult> duplicate = {
            {root / "a" / "same.jpg", "same.jpg"},
            {root / "b" / "same.jpg", "same.jpg"},
        };
        const auto collisions = redactly::findOutputConflicts(duplicate, output);
        assert(collisions.size() == 1);
        assert(collisions[0].kind == redactly::OutputConflict::Kind::DuplicateDestination);
    }

    void testWorkerReportsUnredactedOutputAsWarningAndPreservesIt()
    {
        QTemporaryDir temp;
        assert(temp.isValid());
        const auto root = std::filesystem::path(temp.path().toStdString());
        const auto source = root / "input.png";
        const auto output = root / "out";
        assert(cv::imwrite(source.string(), cv::Mat(24, 24, CV_8UC3, cv::Scalar(20, 40, 60))));

        auto makeWorker = [&]
        {
            redactly::ProcessingRequest request;
            request.inputs = {QString::fromStdString(source.string())};
            request.outputDirectory = QString::fromStdString(output.string());
            request.recursive = false;
            request.detectFaces = false;
            return std::make_unique<redactly::ProcessorWorker>(std::move(request));
        };

        redactly::RunOutcome firstOutcome = redactly::RunOutcome::Failed;
        redactly::RunSummary firstSummary;
        auto first = makeWorker();
        QObject::connect(first.get(), &redactly::ProcessorWorker::summaryAvailable,
                         [&](const redactly::RunSummary summary) { firstSummary = summary; });
        QObject::connect(first.get(), &redactly::ProcessorWorker::finished,
                         [&](const redactly::RunOutcome outcome) { firstOutcome = outcome; });
        first->process();
        assert(firstOutcome == redactly::RunOutcome::CompletedWithWarnings);
        assert(firstSummary.total == 1 && firstSummary.redacted == 0 &&
               firstSummary.unredacted == 1);
        assert(std::filesystem::exists(output / "input.png"));

        const auto savedSize = std::filesystem::file_size(output / "input.png");
        redactly::RunOutcome secondOutcome = redactly::RunOutcome::Completed;
        auto second = makeWorker();
        QObject::connect(second.get(), &redactly::ProcessorWorker::finished,
                         [&](const redactly::RunOutcome outcome) { secondOutcome = outcome; });
        second->process();
        assert(secondOutcome == redactly::RunOutcome::Failed);
        assert(std::filesystem::file_size(output / "input.png") == savedSize);
    }

    void testWorkerReportsCopiedOriginalAsWarning()
    {
        QTemporaryDir temp;
        assert(temp.isValid());
        const auto root = std::filesystem::path(temp.path().toStdString());
        const auto source = root / "input.png";
        const auto output = root / "out";
        assert(cv::imwrite(source.string(), cv::Mat(24, 24, CV_8UC3,
                                                    cv::Scalar(20, 40, 60))));

        qRegisterMetaType<redactly::ReviewResult>("redactly::ReviewResult");
        QThread reviewThread;
        auto *reviewer = new CopyOriginalReviewer;
        reviewer->moveToThread(&reviewThread);
        QObject::connect(&reviewThread, &QThread::finished,
                         reviewer, &QObject::deleteLater);
        reviewThread.start();

        redactly::ProcessingRequest request;
        request.inputs = {QString::fromStdString(source.string())};
        request.outputDirectory = QString::fromStdString(output.string());
        request.detectFaces = false;
        request.reviewEnabled = true;
        request.reviewReceiver = reviewer;

        redactly::RunOutcome result = redactly::RunOutcome::Failed;
        redactly::RunSummary summary;
        redactly::ProcessorWorker worker(std::move(request));
        QObject::connect(&worker, &redactly::ProcessorWorker::summaryAvailable,
                         [&](const redactly::RunSummary value) { summary = value; });
        QObject::connect(&worker, &redactly::ProcessorWorker::finished,
                         [&](const redactly::RunOutcome value) { result = value; });
        worker.process();

        reviewThread.quit();
        assert(reviewThread.wait());
        assert(result == redactly::RunOutcome::CompletedWithWarnings);
        assert(summary.total == 1 && summary.copied == 1);
        assert(std::filesystem::exists(output / "input.png"));
    }

    void testWorkerUsesStableImageSnapshotDuringReview()
    {
        QTemporaryDir temp;
        assert(temp.isValid());
        const auto root = std::filesystem::path(temp.path().toStdString());
        const auto source = root / "input.png";
        const auto replacement = root / "replacement.png";
        const auto output = root / "out";
        assert(cv::imwrite(source.string(), cv::Mat(32, 32, CV_8UC3,
                                                    cv::Scalar(10, 20, 30))));
        assert(cv::imwrite(replacement.string(), cv::Mat(32, 32, CV_8UC3,
                                                         cv::Scalar(200, 210, 220))));
        QFile originalFile(QString::fromStdString(source.string()));
        assert(originalFile.open(QIODevice::ReadOnly));
        const QByteArray originalBytes = originalFile.readAll();
        originalFile.close();

        QThread reviewThread;
        auto *reviewer = new ReplacingCopyOriginalReviewer(
            QString::fromStdString(source.string()),
            QString::fromStdString(replacement.string()));
        reviewer->moveToThread(&reviewThread);
        QObject::connect(&reviewThread, &QThread::finished,
                         reviewer, &QObject::deleteLater);
        reviewThread.start();

        redactly::ProcessingRequest request;
        request.inputs = {QString::fromStdString(source.string())};
        request.outputDirectory = QString::fromStdString(output.string());
        request.detectFaces = false;
        request.reviewEnabled = true;
        request.reviewReceiver = reviewer;
        request.preserveMetadata = true;

        redactly::RunOutcome result = redactly::RunOutcome::Failed;
        redactly::ProcessorWorker worker(std::move(request));
        QObject::connect(&worker, &redactly::ProcessorWorker::finished,
                         [&](const redactly::RunOutcome value) { result = value; });
        worker.process();

        reviewThread.quit();
        assert(reviewThread.wait());
        assert(result == redactly::RunOutcome::CompletedWithWarnings);
        QFile outputFile(QString::fromStdString((output / "input.png").string()));
        assert(outputFile.open(QIODevice::ReadOnly));
        assert(outputFile.readAll() == originalBytes);
    }

    void testWorkerAcceptsThirtyMegabyteJpeg()
    {
        QTemporaryDir temp;
        assert(temp.isValid());
        const auto root = std::filesystem::path(temp.path().toStdString());
        const auto source = root / "large.jpg";
        const auto output = root / "out";
        assert(cv::imwrite(source.string(), cv::Mat(64, 64, CV_8UC3,
                                                    cv::Scalar(40, 80, 120))));
        std::filesystem::resize_file(source, 31ULL * 1024ULL * 1024ULL);

        redactly::ProcessingRequest request;
        request.inputs = {QString::fromStdString(source.string())};
        request.outputDirectory = QString::fromStdString(output.string());
        request.detectFaces = false;

        redactly::RunOutcome result = redactly::RunOutcome::Failed;
        redactly::ProcessorWorker worker(std::move(request));
        QObject::connect(&worker, &redactly::ProcessorWorker::finished,
                         [&](const redactly::RunOutcome value) { result = value; });
        worker.process();
        assert(result == redactly::RunOutcome::CompletedWithWarnings);
        assert(std::filesystem::exists(output / "large.jpg"));
    }

    void testWorkerRejectsMultiFrameImages()
    {
        QTemporaryDir temp;
        assert(temp.isValid());
        const auto root = std::filesystem::path(temp.path().toStdString());
        const auto source = root / "pages.tiff";
        const auto output = root / "out";

        const std::vector<cv::Mat> pages = {
            cv::Mat(12, 16, CV_8UC3, cv::Scalar(20, 40, 60)),
            cv::Mat(12, 16, CV_8UC3, cv::Scalar(80, 100, 120)),
        };
        assert(cv::imwritemulti(source.string(), pages));
        assert(redactly::imageFrameCount(source) == 2);

        redactly::ProcessingRequest request;
        request.inputs = {QString::fromStdString(source.string())};
        request.outputDirectory = QString::fromStdString(output.string());
        request.detectFaces = false;

        redactly::RunOutcome result = redactly::RunOutcome::Failed;
        redactly::RunSummary summary;
        QStringList messages;
        redactly::ProcessorWorker worker(std::move(request));
        QObject::connect(&worker, &redactly::ProcessorWorker::summaryAvailable,
                         [&](const redactly::RunSummary value) { summary = value; });
        QObject::connect(&worker, &redactly::ProcessorWorker::logMessage,
                         [&](const QString &message) { messages.push_back(message); });
        QObject::connect(&worker, &redactly::ProcessorWorker::finished,
                         [&](const redactly::RunOutcome value) { result = value; });
        worker.process();

        assert(result == redactly::RunOutcome::CompletedWithWarnings);
        assert(summary.total == 1 && summary.skipped == 1);
        assert(!std::filesystem::exists(output / "pages.tiff"));
        assert(std::ranges::any_of(messages, [](const QString &message)
        {
            return message.contains("multi-page", Qt::CaseInsensitive);
        }));
    }

    void testAnimatedImageContainersAreDetectedWithoutDecodingAllFrames()
    {
        QTemporaryDir temp;
        assert(temp.isValid());
        const auto root = std::filesystem::path(temp.path().toStdString());

        const auto apng = root / "animated.png";
        writeBytes(apng, {
            0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
            0x00, 0x00, 0x00, 0x08, 'a', 'c', 'T', 'L',
            0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
        });
        assert(redactly::imageFrameCount(apng) > 1);

        const auto webp = root / "animated.webp";
        writeBytes(webp, {
            'R', 'I', 'F', 'F', 0x12, 0x00, 0x00, 0x00,
            'W', 'E', 'B', 'P',
            'V', 'P', '8', 'X', 0x0A, 0x00, 0x00, 0x00,
            0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        });
        assert(redactly::imageFrameCount(webp) > 1);

        const auto bigTiff = root / "pages-bigtiff.tiff";
        writeBytes(bigTiff, {
            'I', 'I', 0x2B, 0x00, 0x08, 0x00, 0x00, 0x00,
            0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        });
        assert(redactly::imageFrameCount(bigTiff) > 1);
    }

    void testApplyMosaicTouchesOnlyDetectedRegion()
    {
        cv::Mat image(8, 8, CV_8UC3);
        for (int y = 0; y < image.rows; ++y)
        {
            for (int x = 0; x < image.cols; ++x)
            {
                image.at<cv::Vec3b>(y, x) = cv::Vec3b(static_cast<uchar>(x * 20),
                                                      static_cast<uchar>(y * 20),
                                                      static_cast<uchar>((x + y) * 10));
            }
        }

        const cv::Vec3b outsideBefore = image.at<cv::Vec3b>(0, 0);
        const cv::Vec3b insideBefore = image.at<cv::Vec3b>(3, 3);

        redactly::FaceDetections detections;
        detections.push_back({cv::Rect2f(2.0F, 2.0F, 4.0F, 4.0F), 1.0F});
        redactly::applyMosaic(image, detections, 4, 0.0F);

        assert(image.at<cv::Vec3b>(0, 0) == outsideBefore);
        assert(image.at<cv::Vec3b>(3, 3) != insideBefore);
    }

    void testSoftEdgesKeepDetectedRegionFullyCovered()
    {
        cv::Mat image(64, 64, CV_8UC3, cv::Scalar(100, 100, 100));
        const cv::Rect box(24, 24, 16, 16);

        redactly::FaceDetections detections;
        detections.push_back({cv::Rect2f(box), 1.0F});
        redactly::applyAnonymization(image, detections, redactly::AnonymizationMethod::Fill,
                                     4, 0.0F, redactly::MaskShape::Rectangle, true);

        for (int y = box.y; y < box.y + box.height; ++y)
        {
            for (int x = box.x; x < box.x + box.width; ++x)
            {
                assert(image.at<cv::Vec3b>(y, x) == cv::Vec3b(0, 0, 0));
            }
        }

        assert(image.at<cv::Vec3b>(0, 0) == cv::Vec3b(100, 100, 100));

        const cv::Vec3b feathered = image.at<cv::Vec3b>(box.y + box.height / 2, box.x + box.width + 1);
        assert(feathered != cv::Vec3b(0, 0, 0));
        assert(feathered != cv::Vec3b(100, 100, 100));
    }

    void testSoftEdgesEllipseKeepsCoreCovered()
    {
        cv::Mat image(64, 64, CV_8UC3, cv::Scalar(100, 100, 100));
        const cv::Rect box(20, 20, 24, 24);

        redactly::FaceDetections detections;
        detections.push_back({cv::Rect2f(box), 1.0F});
        redactly::applyAnonymization(image, detections, redactly::AnonymizationMethod::Fill,
                                     4, 0.0F, redactly::MaskShape::Ellipse, true);

        const int centerX = box.x + box.width / 2;
        const int centerY = box.y + box.height / 2;
        for (int dy = -box.height / 4; dy <= box.height / 4; ++dy)
        {
            for (int dx = -box.width / 4; dx <= box.width / 4; ++dx)
            {
                assert(image.at<cv::Vec3b>(centerY + dy, centerX + dx) == cv::Vec3b(0, 0, 0));
            }
        }

        assert(image.at<cv::Vec3b>(0, 0) == cv::Vec3b(100, 100, 100));
    }

    void testSoftEdgesAtImageBorderStayInBounds()
    {
        cv::Mat image(32, 32, CV_8UC3, cv::Scalar(100, 100, 100));

        redactly::FaceDetections detections;
        detections.push_back({cv::Rect2f(0.0F, 0.0F, 12.0F, 12.0F), 1.0F});
        detections.push_back({cv::Rect2f(24.0F, 24.0F, 8.0F, 8.0F), 1.0F});
        redactly::applyAnonymization(image, detections, redactly::AnonymizationMethod::Fill,
                                     4, 0.0F, redactly::MaskShape::Rectangle, true);

        assert(image.at<cv::Vec3b>(0, 0) == cv::Vec3b(0, 0, 0));
        assert(image.at<cv::Vec3b>(31, 31) == cv::Vec3b(0, 0, 0));
    }

    void testSoftEdgesUsePaddingForAGradualTransition()
    {
        cv::Mat image(96, 96, CV_8UC3, cv::Scalar(100, 100, 100));
        const cv::Rect box(32, 32, 32, 32);

        redactly::FaceDetections detections;
        detections.push_back({cv::Rect2f(box), 1.0F});
        redactly::applyAnonymization(image, detections, redactly::AnonymizationMethod::Fill,
                                     4, 0.25F, redactly::MaskShape::Rectangle, true);

        for (int y = box.y; y < box.y + box.height; ++y)
        {
            for (int x = box.x; x < box.x + box.width; ++x)
            {
                assert(image.at<cv::Vec3b>(y, x) == cv::Vec3b(0, 0, 0));
            }
        }

        const cv::Vec3b innerBlend = image.at<cv::Vec3b>(48, 25);
        assert(innerBlend != cv::Vec3b(0, 0, 0));
        assert(innerBlend != cv::Vec3b(100, 100, 100));

        const cv::Vec3b outerBlend = image.at<cv::Vec3b>(48, 23);
        assert(outerBlend != cv::Vec3b(0, 0, 0));
        assert(outerBlend != cv::Vec3b(100, 100, 100));
        assert(image.at<cv::Vec3b>(48, 16) == cv::Vec3b(100, 100, 100));
    }

    void testLargeSoftEdgeMaskUsesBoundedWorkingMemoryPath()
    {
        cv::Mat image(1500, 1500, CV_8UC3, cv::Scalar(100, 100, 100));
        redactly::FaceDetections detections;
        detections.push_back({cv::Rect2f(100.0F, 100.0F, 1300.0F, 1300.0F), 1.0F});

        redactly::applyAnonymization(image, detections, redactly::AnonymizationMethod::Fill,
                                     8, 0.0F, redactly::MaskShape::Rectangle, true);

        assert(image.at<cv::Vec3b>(750, 750) == cv::Vec3b(0, 0, 0));
        assert(image.at<cv::Vec3b>(0, 0) == cv::Vec3b(100, 100, 100));
        const auto feathered = image.at<cv::Vec3b>(750, 30);
        assert(feathered != cv::Vec3b(0, 0, 0));
        assert(feathered != cv::Vec3b(100, 100, 100));
    }

    void testStickerCoversDetectedRegion()
    {
        cv::Mat image(96, 96, CV_8UC3, cv::Scalar(100, 100, 100));
        const cv::Rect box(32, 32, 32, 32);

        redactly::FaceDetections detections;
        detections.push_back({cv::Rect2f(box), 1.0F});
        redactly::applyAnonymization(image, detections,
                                     redactly::AnonymizationMethod::Sticker,
                                     4, 0.0F, redactly::MaskShape::Ellipse, true);

        for (int y = box.y; y < box.y + box.height; ++y)
        {
            for (int x = box.x; x < box.x + box.width; ++x)
            {
                assert(image.at<cv::Vec3b>(y, x) != cv::Vec3b(100, 100, 100));
            }
        }
        assert(image.at<cv::Vec3b>(0, 0) == cv::Vec3b(100, 100, 100));
        assert(image.at<cv::Vec3b>(48, 48) != image.at<cv::Vec3b>(44, 43));
    }

    void testOrientationTransforms()
    {
        cv::Mat base(2, 3, CV_8UC1);
        for (int r = 0; r < base.rows; ++r)
        {
            for (int c = 0; c < base.cols; ++c)
            {
                base.at<uchar>(r, c) = static_cast<uchar>(r * 10 + c);
            }
        }

        cv::Mat identity = base.clone();
        redactly::applyOrientation(identity, 1);
        assert(cv::countNonZero(identity != base) == 0);

        cv::Mat rotated = base.clone();
        redactly::applyOrientation(rotated, 6);
        assert(rotated.rows == 3 && rotated.cols == 2);
        assert(rotated.at<uchar>(0, 0) == base.at<uchar>(base.rows - 1, 0));

        cv::Mat mirrored = base.clone();
        redactly::applyOrientation(mirrored, 2);
        assert(mirrored.rows == 2 && mirrored.cols == 3);
        assert(mirrored.at<uchar>(0, 0) == base.at<uchar>(0, base.cols - 1));
    }

    void testExifOrientationFallback()
    {
        QTemporaryDir temp;
        assert(temp.isValid());
        const auto root = std::filesystem::path(temp.path().toStdString());
        for (unsigned char orientation = 1; orientation <= 8; ++orientation)
        {
            const auto path = root / ("orientation-" + std::to_string(orientation) + ".jpg");
            writeJpegWithExifOrientation(path, orientation);
            assert(redactly::readExifOrientation(path) == orientation);
        }

        const auto path = root / "orientation-6.jpg";
        cv::Mat image = redactly::imreadUnicode(path, cv::IMREAD_UNCHANGED);
        assert(image.rows == 2 && image.cols == 3);
        redactly::applyOrientation(image, redactly::readExifOrientation(path));
        assert(image.rows == 3 && image.cols == 2);
    }

    void testEncodeParams()
    {
        const auto jpeg = redactly::encodeParamsForExtension(".JPG");
        assert(std::find(jpeg.begin(), jpeg.end(), cv::IMWRITE_JPEG_QUALITY) != jpeg.end());
        assert(std::find(jpeg.begin(), jpeg.end(), 100) != jpeg.end());

        const auto png = redactly::encodeParamsForExtension("png");
        assert(std::find(png.begin(), png.end(), cv::IMWRITE_PNG_COMPRESSION) != png.end());

        assert(redactly::encodeParamsForExtension(".bmp").empty());
    }

    void testImageWritePublishesWithoutReplacing()
    {
        QTemporaryDir temp;
        assert(temp.isValid());
        const auto root = std::filesystem::path(temp.path().toStdString());
        const auto destination = root / "result.png";

        writeBytes(QString::fromStdString(destination.string()));
        const cv::Mat first(128, 128, CV_8UC3, cv::Scalar(20, 40, 60));
        assert(!redactly::imwriteUnicodeNoReplace(destination, first));
        assert(std::filesystem::file_size(destination) == 1);
        assert(std::filesystem::remove(destination));

        std::barrier start(3);
        std::atomic<int> successes{0};
        const cv::Mat second(512, 512, CV_8UC3, cv::Scalar(80, 100, 120));
        const auto write = [&](const cv::Mat &image)
        {
            start.arrive_and_wait();
            if (redactly::imwriteUnicodeNoReplace(destination, image))
            {
                successes.fetch_add(1, std::memory_order_relaxed);
            }
        };
        std::thread firstWriter(write, std::cref(first));
        std::thread secondWriter(write, std::cref(second));
        start.arrive_and_wait();
        firstWriter.join();
        secondWriter.join();

        assert(successes.load(std::memory_order_relaxed) == 1);
        assert(!cv::imread(destination.string(), cv::IMREAD_UNCHANGED).empty());
        for (const auto &entry: std::filesystem::directory_iterator(root))
        {
            assert(entry.path().filename().string().find(".redactly-") == std::string::npos);
        }
    }

    void testRootedWritesRejectEscapesAndUsePrivateFiles()
    {
        QTemporaryDir temp;
        assert(temp.isValid());
        const auto base = std::filesystem::canonical(
            std::filesystem::path(temp.path().toStdString()));
        const auto root = base / "output";
        const auto outside = base / "outside";
        assert(std::filesystem::create_directories(root));
        assert(std::filesystem::create_directories(outside));

        const cv::Mat image(32, 32, CV_8UC3, cv::Scalar(30, 60, 90));
        assert(redactly::imwriteUnicodeNoReplaceAtRoot(
                   root, "nested/result.png", image) == redactly::ImageWriteResult::Saved);
        assert(!cv::imread((root / "nested/result.png").string()).empty());
        assert(redactly::imwriteUnicodeNoReplaceAtRoot(
                   root, "../outside/escaped.png", image) == redactly::ImageWriteResult::Failed);
        assert(!std::filesystem::exists(outside / "escaped.png"));

        const auto source = base / "original.bin";
        const std::vector<uchar> sourceBytes = {0, 1, 2, 3, 4, 5, 0xFE, 0xFF};
        writeBytes(source, sourceBytes);
#ifndef _WIN32
        assert(::chmod(source.c_str(), 0600) == 0);
#endif
        assert(redactly::copyFileNoReplaceAtRoot(source, root, "copies/original.bin"));
        std::ifstream copied(root / "copies/original.bin", std::ios::binary);
        const std::istreambuf_iterator<char> copiedBegin(copied);
        const std::istreambuf_iterator<char> copiedEnd;
        const std::vector<uchar> copiedBytes(copiedBegin, copiedEnd);
        assert(copiedBytes == sourceBytes);

        const auto moveSource = base / "move-source.bin";
        writeBytes(moveSource, sourceBytes);
        assert(redactly::moveFileNoReplaceAtRoot(
                   moveSource, root, "moves/moved.bin") ==
               redactly::FileMoveResult::Moved);
        assert(!std::filesystem::exists(moveSource));
        std::ifstream moved(root / "moves/moved.bin", std::ios::binary);
        const std::istreambuf_iterator<char> movedBegin(moved);
        const std::istreambuf_iterator<char> movedEnd;
        const std::vector<uchar> movedBytes(movedBegin, movedEnd);
        assert(movedBytes == sourceBytes);

        const auto blockedSource = base / "blocked-source.bin";
        const auto blockedDestination = root / "moves/existing.bin";
        const std::vector<uchar> existingBytes = {9, 8, 7, 6};
        writeBytes(blockedSource, sourceBytes);
        writeBytes(blockedDestination, existingBytes);
#ifndef _WIN32
        assert(::chmod(blockedSource.c_str(), 0640) == 0);
#endif
        assert(redactly::moveFileNoReplaceAtRoot(
                   blockedSource, root, "moves/existing.bin") ==
               redactly::FileMoveResult::Failed);
        assert(std::filesystem::exists(blockedSource));
        std::ifstream existing(blockedDestination, std::ios::binary);
        const std::istreambuf_iterator<char> existingBegin(existing);
        const std::istreambuf_iterator<char> existingEnd;
        const std::vector<uchar> preservedBytes(existingBegin, existingEnd);
        assert(preservedBytes == existingBytes);
#ifndef _WIN32
        struct stat blockedStatus{};
        assert(::stat(blockedSource.c_str(), &blockedStatus) == 0);
        assert((blockedStatus.st_mode & 0777) == 0640);
#endif

        const auto guardedSource = base / "guarded-source.bin";
        writeBytes(guardedSource, sourceBytes);
        assert(redactly::moveFileNoReplaceAtRoot(
                   guardedSource, root, "moves/guarded.bin", []
                   {
                       return false;
                   }) == redactly::FileMoveResult::Failed);
        assert(std::filesystem::exists(guardedSource));
        assert(!std::filesystem::exists(root / "moves/guarded.bin"));
#ifndef _WIN32
        struct stat copiedStatus{};
        assert(::stat((root / "copies/original.bin").c_str(), &copiedStatus) == 0);
        assert((copiedStatus.st_mode & 0777) == 0600);

        std::error_code ec;
        std::filesystem::create_directory_symlink(outside, root / "linked", ec);
        assert(!ec);
        assert(redactly::imwriteUnicodeNoReplaceAtRoot(
                   root, "linked/escaped.png", image) == redactly::ImageWriteResult::Failed);
        assert(!std::filesystem::exists(outside / "escaped.png"));
#endif

        for (const auto &entry: std::filesystem::recursive_directory_iterator(root))
        {
            assert(!entry.path().filename().string().starts_with(".redactly-"));
        }
    }

    void testMetadataFailurePublishesCleanImage()
    {
        QTemporaryDir temp;
        assert(temp.isValid());
        const auto root = std::filesystem::canonical(
            std::filesystem::path(temp.path().toStdString()));
        const cv::Mat image(24, 24, CV_8UC3, cv::Scalar(15, 45, 75));
        const auto result = redactly::imwriteUnicodeNoReplaceAtRoot(
            root, "result.jpg", image, redactly::encodeParamsForExtension("jpg"),
            root / "missing-metadata-source.jpg");
        assert(result == redactly::ImageWriteResult::SavedWithoutMetadata);
        assert(!cv::imread((root / "result.jpg").string()).empty());
    }

    void testIntersectionOverUnion()
    {
        const cv::Rect2f a(0.0F, 0.0F, 10.0F, 10.0F);
        assert(std::abs(redactly::intersectionOverUnion(a, a) - 1.0F) < 1e-5F);

        const cv::Rect2f disjoint(100.0F, 100.0F, 10.0F, 10.0F);
        assert(redactly::intersectionOverUnion(a, disjoint) == 0.0F);

        const cv::Rect2f empty(0.0F, 0.0F, 0.0F, 0.0F);
        assert(redactly::intersectionOverUnion(a, empty) == 0.0F);

        const cv::Rect2f halfShifted(5.0F, 0.0F, 10.0F, 10.0F);
        assert(std::abs(redactly::intersectionOverUnion(a, halfShifted) - (50.0F / 150.0F)) < 1e-5F);
    }

    void testNonMaxSuppression()
    {
        redactly::FaceDetections detections;
        detections.push_back({cv::Rect2f(0.0F, 0.0F, 10.0F, 10.0F), 0.9F});
        detections.push_back({cv::Rect2f(1.0F, 1.0F, 10.0F, 10.0F), 0.8F});
        detections.push_back({cv::Rect2f(100.0F, 100.0F, 10.0F, 10.0F), 0.7F});

        const auto kept = redactly::nonMaxSuppression(detections, 0.4F);
        assert(kept.size() == 2);
        assert(kept[0].score == 0.9F);
        assert(kept[1].box.x == 100.0F);
    }

    void testInvalidDetectionsAreIgnored()
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        redactly::FaceDetections detections = {
            {cv::Rect2f(nan, 0.0F, 10.0F, 10.0F), 0.9F},
            {cv::Rect2f(0.0F, 0.0F, 10.0F, 10.0F), nan},
            {cv::Rect2f(2.0F, 2.0F, 8.0F, 8.0F), 0.8F},
        };
        const auto kept = redactly::nonMaxSuppression(detections, 0.4F);
        assert(kept.size() == 1);

        cv::Mat image(16, 16, CV_8UC3, cv::Scalar(30, 60, 90));
        const cv::Mat before = image.clone();
        redactly::applyAnonymization(
            image, {{cv::Rect2f(0.0F, nan, 10.0F, 10.0F), 0.9F}},
            redactly::AnonymizationMethod::Fill, 8, 0.0F);
        assert(cv::norm(image, before, cv::NORM_INF) == 0.0);
    }

    void testDestinationPathSafety()
    {
        QTemporaryDir temp;
        assert(temp.isValid());
        const std::filesystem::path root =
            std::filesystem::path(temp.path().toStdString()) / "out";
        assert(std::filesystem::create_directories(root));

        assert(redactly::destinationIsSafe(root / "a.jpg", root));
        assert(redactly::destinationIsSafe(root / "sub" / "b.png", root));
        assert(redactly::destinationIsSafe(root, root));

        assert(!redactly::destinationIsSafe(root / ".." / "escape.jpg", root));

        assert(redactly::isWithinRoot(root / "x.jpg", root));
        assert(!redactly::isWithinRoot(root.parent_path() / "x.jpg", root));
    }

#ifndef _WIN32
    void testDestinationRejectsSymlinkEscape()
    {
        QTemporaryDir temp;
        assert(temp.isValid());
        const std::filesystem::path base = std::filesystem::path(temp.path().toStdString());
        const std::filesystem::path root = base / "out";
        const std::filesystem::path outside = base / "outside";
        assert(std::filesystem::create_directories(root));
        assert(std::filesystem::create_directories(outside));

        std::error_code ec;
        const std::filesystem::path link = root / "evil";
        std::filesystem::create_directory_symlink(outside, link, ec);
        assert(!ec);

        assert(!redactly::destinationIsSafe(link / "leak.jpg", root));

        const std::filesystem::path danglingTarget = outside / "not-created.jpg";
        const std::filesystem::path danglingLink = root / "dangling.jpg";
        std::filesystem::create_symlink(danglingTarget, danglingLink, ec);
        assert(!ec);
        assert(!std::filesystem::exists(danglingLink));
        assert(std::filesystem::is_symlink(std::filesystem::symlink_status(danglingLink)));
        assert(!redactly::destinationIsSafe(danglingLink, root));

        const std::vector<redactly::ScanResult> planned = {
            {base / "input" / "dangling.jpg", "dangling.jpg"},
        };
        const auto conflicts = redactly::findOutputConflicts(planned, root);
        assert(conflicts.size() == 1);
        assert(conflicts.front().kind ==
               redactly::OutputConflict::Kind::ExistingDestination);

        const auto source = base / "source.jpg";
        assert(cv::imwrite(source.string(), cv::Mat(8, 8, CV_8UC3, cv::Scalar(1, 2, 3))));
        assert(!redactly::copyFileNoReplace(source, danglingLink));
        assert(!redactly::imwriteUnicodeNoReplace(
            danglingLink, cv::Mat(8, 8, CV_8UC3, cv::Scalar(4, 5, 6))));
        assert(!std::filesystem::exists(danglingTarget));
    }
#endif

#ifdef REDACTLY_HAVE_EXIV2
    void testMetadataCopyAndOrientationNormalize()
    {
        QTemporaryDir temp;
        assert(temp.isValid());
        const auto root = std::filesystem::canonical(
            std::filesystem::path(temp.path().toStdString()));
        const std::filesystem::path src = root / "src.jpg";
        const std::filesystem::path dst = root / "dst.jpg";

        cv::Mat img(16, 16, CV_8UC3, cv::Scalar(120, 120, 120));
        assert(cv::imwrite(src.string(), img));
        assert(cv::imwrite(dst.string(), img));

        {
            auto image = Exiv2::ImageFactory::open(src.string());
            image->readMetadata();
            image->exifData()["Exif.Image.Artist"] = "TestPhotographer";
            image->exifData()["Exif.Image.Orientation"] = static_cast<uint16_t>(6);
            image->exifData()["Exif.Photo.UserComment"] =
                "data:image/jpeg;base64,unsafe-payload";
            image->xmpData()["Xmp.tiff.Orientation"] = "6";
            image->xmpData()["Xmp.dc.description"] =
                "data:image/jpeg;base64,unsafe-payload";
            image->setComment("data:image/jpeg;base64,unsafe-payload");
            image->writeMetadata();
        }

        assert(redactly::readExifOrientation(src) == 6);
        assert(redactly::copyMetadata(src, dst, true));

        {
            auto image = Exiv2::ImageFactory::open(dst.string());
            image->readMetadata();
            const Exiv2::ExifData &exif = image->exifData();

            const auto artist = exif.findKey(Exiv2::ExifKey("Exif.Image.Artist"));
            assert(artist != exif.end());
            assert(artist->toString() == "TestPhotographer");

            const auto orientation = exif.findKey(Exiv2::ExifKey("Exif.Image.Orientation"));
            assert(orientation != exif.end());
#if EXIV2_TEST_VERSION(0, 28, 0)
            assert(orientation->toInt64() == 1);
#else
            assert(orientation->toLong() == 1);
#endif
            assert(image->xmpData().findKey(Exiv2::XmpKey("Xmp.tiff.Orientation")) ==
                   image->xmpData().end());
            assert(image->xmpData().empty());
            assert(image->exifData().findKey(
                       Exiv2::ExifKey("Exif.Photo.UserComment")) ==
                   image->exifData().end());
            assert(image->comment().empty());
        }

        const auto published = root / "published.jpg";
        assert(redactly::imwriteUnicodeNoReplaceAtRoot(
                   root, published.filename(), img,
                   redactly::encodeParamsForExtension("jpg"), src) ==
               redactly::ImageWriteResult::Saved);
        auto publishedMetadata = Exiv2::ImageFactory::open(published.string());
        publishedMetadata->readMetadata();
        const auto artist = publishedMetadata->exifData().findKey(
            Exiv2::ExifKey("Exif.Image.Artist"));
        assert(artist != publishedMetadata->exifData().end());
        assert(artist->toString() == "TestPhotographer");
        assert(publishedMetadata->xmpData().findKey(
                   Exiv2::XmpKey("Xmp.tiff.Orientation")) ==
               publishedMetadata->xmpData().end());
    }

    long exifThumbnailBytes(Exiv2::ExifData &exif)
    {
        Exiv2::ExifThumb thumb(exif);
#if EXIV2_TEST_VERSION(0, 28, 0)
        return static_cast<long>(thumb.copy().size());
#else
        return thumb.copy().size_;
#endif
    }

    void testMetadataCopyStripsEmbeddedThumbnail()
    {
        QTemporaryDir temp;
        assert(temp.isValid());
        const std::filesystem::path base = std::filesystem::path(temp.path().toStdString());
        const std::filesystem::path src = base / "src.jpg";
        const std::filesystem::path dst = base / "dst.jpg";
        const std::filesystem::path thumbFile = base / "thumb.jpg";

        cv::Mat img(16, 16, CV_8UC3, cv::Scalar(120, 120, 120));
        assert(cv::imwrite(src.string(), img));
        assert(cv::imwrite(dst.string(), img));
        assert(cv::imwrite(thumbFile.string(), img));

        {
            auto image = Exiv2::ImageFactory::open(src.string());
            image->readMetadata();
            image->exifData()["Exif.Image.Artist"] = "TestPhotographer";
            Exiv2::ExifThumb thumb(image->exifData());
            thumb.setJpegThumbnail(thumbFile.string());
            image->writeMetadata();
        }

        {
            auto image = Exiv2::ImageFactory::open(src.string());
            image->readMetadata();
            assert(exifThumbnailBytes(image->exifData()) > 0);
        }

        assert(redactly::copyMetadata(src, dst, true));

        {
            auto image = Exiv2::ImageFactory::open(dst.string());
            image->readMetadata();
            assert(exifThumbnailBytes(image->exifData()) == 0);

            const Exiv2::ExifData &exif = image->exifData();
            const auto artist = exif.findKey(Exiv2::ExifKey("Exif.Image.Artist"));
            assert(artist != exif.end());
            assert(artist->toString() == "TestPhotographer");
        }
    }
#endif
}

namespace
{
    void testDetectorsRejectUnexpectedModelHash()
    {
        QTemporaryDir temp;
        assert(temp.isValid());
        const QString modelPath = temp.filePath("model.onnx");
        QFile model(modelPath);
        assert(model.open(QIODevice::WriteOnly));
        assert(model.write("invalid-model") == 13);
        model.close();

        const QByteArray unexpectedHash(32, '\0');
        bool faceRejected = false;
        try
        {
            redactly::ScrfdFaceDetector detector(modelPath.toStdString(), 640, false,
                                                  unexpectedHash);
        }
        catch (const std::runtime_error &error)
        {
            faceRejected = std::string(error.what()).find("changed") != std::string::npos;
        }
        assert(faceRejected);

        bool plateRejected = false;
        try
        {
            redactly::PlateDetector detector(modelPath.toStdString(), false, unexpectedHash);
        }
        catch (const std::runtime_error &error)
        {
            plateRejected = std::string(error.what()).find("changed") != std::string::npos;
        }
        assert(plateRejected);
    }

    void testOnnxPatchRejectsInvalidBytes()
    {
        assert(!redactly::makeOnnxSpatialDimsFixed({}, 320).has_value());
        const std::vector<std::uint8_t> garbage = {0xFF, 0xFF, 0xFF, 0x01, 0x02, 0x9C};
        assert(!redactly::makeOnnxSpatialDimsFixed(garbage, 320).has_value());
        assert(!redactly::makeOnnxSpatialDimsFixed(garbage, 0).has_value());
        assert(!redactly::makeOnnxSpatialDimsFixed(garbage, -320).has_value());
    }

    void testFixedScrfdModelRunsAtRequestedSize()
    {
        const auto modelPath = std::filesystem::path(__FILE__).parent_path().parent_path()
                               / "models" / "2.5g_bnkps.onnx";
        if (!std::filesystem::exists(modelPath))
        {
            std::puts("skipping fixed-model patch test: models/2.5g_bnkps.onnx not present");
            return;
        }

        redactly::ScrfdFaceDetector nativeSize(modelPath.string(), 640);
        assert(nativeSize.inputSize() == 640);

        redactly::ScrfdFaceDetector patchedSize(modelPath.string(), 320);
        assert(patchedSize.inputSize() == 320);

        const cv::Mat blank(180, 320, CV_8UC3, cv::Scalar(30, 30, 30));
        assert(patchedSize.detect(blank, 0.5F, 0.4F).empty());
    }

    void testDynamicScrfdModelRunsAtRequestedSize()
    {
        const auto modelPath = std::filesystem::path(__FILE__).parent_path().parent_path()
                               / "models" / "10g_bnkps.onnx";
        if (!std::filesystem::exists(modelPath))
        {
            std::puts("skipping dynamic-model patch test: models/10g_bnkps.onnx not present");
            return;
        }

        redactly::ScrfdFaceDetector patchedSize(modelPath.string(), 320);
        assert(patchedSize.inputSize() == 320);

        const cv::Mat blank(180, 320, CV_8UC3, cv::Scalar(30, 30, 30));
        assert(patchedSize.detect(blank, 0.5F, 0.4F).empty());
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testAccelerationBackendNames();
    testBuiltinModelDigests();
    testSupportedImageExtensions();
    testScanImagesRecursesAndDeduplicates();
    testOutputPlanRejectsExistingAndDuplicateDestinations();
    testWorkerReportsUnredactedOutputAsWarningAndPreservesIt();
    testWorkerReportsCopiedOriginalAsWarning();
    testWorkerUsesStableImageSnapshotDuringReview();
    testWorkerAcceptsThirtyMegabyteJpeg();
    testWorkerRejectsMultiFrameImages();
    testAnimatedImageContainersAreDetectedWithoutDecodingAllFrames();
    testApplyMosaicTouchesOnlyDetectedRegion();
    testSoftEdgesKeepDetectedRegionFullyCovered();
    testSoftEdgesEllipseKeepsCoreCovered();
    testSoftEdgesAtImageBorderStayInBounds();
    testSoftEdgesUsePaddingForAGradualTransition();
    testLargeSoftEdgeMaskUsesBoundedWorkingMemoryPath();
    testStickerCoversDetectedRegion();
    testOrientationTransforms();
    testExifOrientationFallback();
    testEncodeParams();
    testImageWritePublishesWithoutReplacing();
    testRootedWritesRejectEscapesAndUsePrivateFiles();
    testMetadataFailurePublishesCleanImage();
    testIntersectionOverUnion();
    testNonMaxSuppression();
    testInvalidDetectionsAreIgnored();
    testDetectorsRejectUnexpectedModelHash();
    testOnnxPatchRejectsInvalidBytes();
    testFixedScrfdModelRunsAtRequestedSize();
    testDynamicScrfdModelRunsAtRequestedSize();
    testDestinationPathSafety();
#ifndef _WIN32
    testDestinationRejectsSymlinkEscape();
#endif
#ifdef REDACTLY_HAVE_EXIV2
    testMetadataCopyAndOrientationNormalize();
    testMetadataCopyStripsEmbeddedThumbnail();
#endif
    return 0;
}

#include "test_core.moc"
