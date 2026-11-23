#include "image_correction.h"

#include "conversion_core.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <opencv2/core.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <array>
#include <cmath>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
bool matrix(const QJsonValue &value, int rows, int cols, cv::Mat *out) {
    const auto outer = value.toArray();
    if (outer.size() != rows) return false;
    *out = cv::Mat(rows, cols, CV_64F);
    for (int row = 0; row < rows; ++row) {
        const auto inner = outer[row].toArray();
        if (inner.size() != cols) return false;
        for (int col = 0; col < cols; ++col) {
            if (!inner[col].isDouble()) return false;
            const double number = inner[col].toDouble();
            if (!std::isfinite(number)) return false;
            out->at<double>(row, col) = number;
        }
    }
    return true;
}

bool distortion(const QJsonValue &value, cv::Mat *out) {
    const auto values = value.toArray();
    if (values.size() != 4) return false;
    *out = cv::Mat(4, 1, CV_64F);
    for (int i = 0; i < 4; ++i) {
        if (!values[i].isDouble() || !std::isfinite(values[i].toDouble())) return false;
        out->at<double>(i) = values[i].toDouble();
    }
    return true;
}

struct ProfileData {
    QByteArray bytes;
    CorrectionProfileInfo info;
    cv::Mat k, d, p;
};

bool loadProfile(const QString &path, ProfileData *data, QString *error) {
    QFile profile(path);
    if (!profile.open(QIODevice::ReadOnly)) { if (error) *error = profile.errorString(); return false; }
    data->bytes = profile.readAll();
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(data->bytes, &parseError);
    const auto object = document.object();
    const auto size = object.value(QStringLiteral("image_size")).toObject();
    data->info = {object.value(QStringLiteral("camera_model")).toString(),
                  object.value(QStringLiteral("distortion_model")).toString(),
                  size.value(QStringLiteral("width")).toInt(), size.value(QStringLiteral("height")).toInt()};
    if (parseError.error != QJsonParseError::NoError || !document.isObject() ||
        data->info.distortionModel != QStringLiteral("opencv_fisheye_kannala_brandt") ||
        data->info.width <= 0 || data->info.height <= 0 ||
        !matrix(object.value(QStringLiteral("K")), 3, 3, &data->k) ||
        !distortion(object.value(QStringLiteral("D")), &data->d) ||
        !matrix(object.value(QStringLiteral("P")), 3, 3, &data->p)) {
        if (error) *error = QStringLiteral("Invalid or unsupported fisheye K/D/P calibration profile.");
        return false;
    }
    return true;
}

int exifOrientation(const QString &path) {
    QImageReader reader(path);
    reader.setAutoTransform(false);
    // Qt's transformation flags correspond to the eight EXIF orientations.
    static constexpr std::array<int, 8> orientations{1, 2, 4, 3, 6, 5, 7, 8};
    const int flags = int(reader.transformation());
    return flags >= 0 && flags < int(orientations.size()) ? orientations[flags] : 0;
}

QByteArray orientationSegment(int orientation) {
    QByteArray segment = QByteArray::fromHex(
        "ffe1002245786966000049492a0008000000010012010300010000000100000000000000");
    segment[28] = char(orientation);
    return segment;
}
}

bool ImageCorrection::inspectProfile(const QString &path, CorrectionProfileInfo *info, QString *error) {
    if (!info) return false;
    ProfileData data;
    if (!loadProfile(path, &data, error)) return false;
    *info = data.info;
    return true;
}

bool ImageCorrection::run(const QString &projectDb, const QString &datasetId, const QString &profilePath,
                          CorrectionResult *result, const std::function<void(int, int)> &progress,
                          QString *error) {
    if (!result) { if (error) *error = QStringLiteral("Missing result object."); return false; }
    *result = {};
    ProfileData profile;
    if (!loadProfile(profilePath, &profile, error)) return false;
    QList<ImportedImage> images;
    if (!ConversionCore::images(projectDb, datasetId, &images, error)) return false;
    const QDir project = QFileInfo(projectDb).absoluteDir();
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(profile.bytes, QCryptographicHash::Sha256).toHex());
    const QString storedProfile = project.filePath(QStringLiteral("source/calibration/") + hash + QStringLiteral(".json"));
    if (!QDir().mkpath(QFileInfo(storedProfile).absolutePath())) {
        if (error) *error = QStringLiteral("Cannot create calibration directory.");
        return false;
    }
    if (!QFileInfo::exists(storedProfile)) {
        QSaveFile target(storedProfile);
        if (!target.open(QIODevice::WriteOnly) || target.write(profile.bytes) != profile.bytes.size() || !target.commit()) {
            if (error) *error = QStringLiteral("Cannot store calibration profile in project.");
            return false;
        }
    }
    const QString outputDir = project.filePath(QStringLiteral("derived/images/device_undistorted/") + datasetId +
                                                QStringLiteral("/") + hash);
    if (!QDir().mkpath(outputDir)) { if (error) *error = QStringLiteral("Cannot create correction output directory."); return false; }

    std::map<std::pair<int, int>, std::pair<cv::Mat, cv::Mat>> maps;
    for (const auto &image : images) {
        try {
            const QString input = project.filePath(image.rawPath);
            QFile source(input);
            if (!source.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read image");
            const QByteArray encoded = source.readAll();
            cv::Mat raw = cv::imdecode(cv::Mat(1, int(encoded.size()), CV_8U, const_cast<char *>(encoded.constData())),
                                      cv::IMREAD_COLOR | cv::IMREAD_IGNORE_ORIENTATION);
            if (raw.empty()) throw std::runtime_error("Cannot decode image");
            const int orientation = exifOrientation(input);
            if (orientation == 0) throw std::runtime_error("Unsupported EXIF orientation");
            const auto key = std::make_pair(raw.cols, raw.rows);
            auto iter = maps.find(key);
            if (iter == maps.end()) {
                cv::Mat scaledK = profile.k.clone(), scaledP = profile.p.clone();
                for (int col = 0; col < 3; ++col) {
                    scaledK.at<double>(0, col) *= double(raw.cols) / profile.info.width;
                    scaledK.at<double>(1, col) *= double(raw.rows) / profile.info.height;
                    scaledP.at<double>(0, col) *= double(raw.cols) / profile.info.width;
                    scaledP.at<double>(1, col) *= double(raw.rows) / profile.info.height;
                }
                std::pair<cv::Mat, cv::Mat> remap;
                cv::fisheye::initUndistortRectifyMap(scaledK, profile.d, cv::Mat::eye(3, 3, CV_64F), scaledP,
                                                     raw.size(), CV_16SC2, remap.first, remap.second);
                iter = maps.emplace(key, std::move(remap)).first;
            }
            cv::Mat corrected;
            cv::remap(raw, corrected, iter->second.first, iter->second.second, cv::INTER_LINEAR, cv::BORDER_CONSTANT);
            std::vector<uchar> jpeg;
            if (!cv::imencode(".jpg", corrected, jpeg, {cv::IMWRITE_JPEG_QUALITY, 95}))
                throw std::runtime_error("Cannot encode JPEG");
            const QString output = project.filePath(ConversionCore::correctedRelativePath(datasetId, hash, image));
            if (!QDir().mkpath(QFileInfo(output).absolutePath())) throw std::runtime_error("Cannot create image output folder");
            QSaveFile target(output);
            if (!target.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot open output");
            if (jpeg.size() < 2 || jpeg[0] != 0xff || jpeg[1] != 0xd8) throw std::runtime_error("Invalid JPEG output");
            const QByteArray exif = orientationSegment(orientation);
            if (target.write(reinterpret_cast<const char *>(jpeg.data()), 2) != 2 ||
                target.write(exif) != exif.size() ||
                target.write(reinterpret_cast<const char *>(jpeg.data() + 2), qint64(jpeg.size() - 2)) != qint64(jpeg.size() - 2) ||
                !target.commit()) throw std::runtime_error("Cannot save output");
            ++result->processed;
        } catch (const std::exception &ex) {
            ++result->failed;
            result->errors.append(image.sourceId + QStringLiteral(": ") + QString::fromUtf8(ex.what()));
        }
        if (progress) progress(result->processed + result->failed, images.size());
    }
    return true;
}
