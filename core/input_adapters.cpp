#include "input_adapters.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {
bool imageSize(const QString &path, int *width, int *height) {
    QImageReader reader(path);
    reader.setAutoTransform(false);
    const QSize size = reader.size();
    *width = size.width();
    *height = size.height();
    return size.isValid();
}

QJsonObject jsonObject(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return {};
    }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = path + QStringLiteral(": ") + parseError.errorString();
        return {};
    }
    return document.object();
}

bool insideDirectory(const QString &directory, const QString &file) {
    const QString root = QDir::fromNativeSeparators(QDir(directory).canonicalPath()) + QStringLiteral("/");
    const QString path = QDir::fromNativeSeparators(QFileInfo(file).canonicalFilePath());
    return !path.isEmpty() && path.startsWith(root, Qt::CaseInsensitive);
}

class InputAdapter {
public:
    virtual ~InputAdapter() = default;
    virtual QString id() const = 0;
    virtual bool detects(const QString &root) const = 0;
    virtual void collect(const QString &root, ScanResult *result) const = 0;
};

class EliosAdapter final : public InputAdapter {
public:
    QString id() const override { return QStringLiteral("ELIOS_INSPECTOR5"); }
    bool detects(const QString &root) const override {
        return QDir(root).exists(QStringLiteral("snapshots")) && QDir(root).exists(QStringLiteral("flights"));
    }
    void collect(const QString &root, ScanResult *result) const override {
        QDir snapshots(QDir(root).filePath(QStringLiteral("snapshots")));
        for (const auto &folder : snapshots.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString jsonPath = QDir(folder.absoluteFilePath()).filePath(QStringLiteral("snapshot.json"));
            if (!QFileInfo::exists(jsonPath)) continue;
            QString parseError;
            const auto snapshot = jsonObject(jsonPath, &parseError);
            if (!parseError.isEmpty()) { result->warnings << parseError; continue; }
            const QString imageName = snapshot.value(QStringLiteral("camera_image")).toString();
            const QString imagePath = QDir(folder.absoluteFilePath()).filePath(imageName);
            SourceImage image;
            image.sourceId = snapshot.value(QStringLiteral("id")).toString();
            image.sourcePath = imagePath;
            image.flightId = snapshot.value(QStringLiteral("flight_id")).toString();
            image.poseJson = QString::fromUtf8(QJsonDocument(snapshot.value(QStringLiteral("camera_pose")).toObject()).toJson(QJsonDocument::Compact));
            if (image.sourceId.isEmpty() || imageName.isEmpty() ||
                !insideDirectory(folder.absoluteFilePath(), imagePath) || !QFileInfo(imagePath).isFile() ||
                !imageSize(imagePath, &image.width, &image.height)) {
                result->warnings << QStringLiteral("Missing or unreadable snapshot image: ") + jsonPath;
                continue;
            }
            if (image.poseJson == QStringLiteral("{}"))
                result->warnings << QStringLiteral("Camera pose missing: ") + image.sourceId;
            result->images.append(image);
        }
        QDir maps(QDir(root).filePath(QStringLiteral("maps")));
        for (const auto &folder : maps.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString mapPath = QDir(folder.absoluteFilePath()).filePath(QStringLiteral("map.json"));
            if (!QFileInfo::exists(mapPath)) continue;
            QString parseError;
            const auto map = jsonObject(mapPath, &parseError);
            if (!parseError.isEmpty()) { result->warnings << parseError; continue; }
            for (const auto value : map.value(QStringLiteral("point_clouds")).toArray()) {
                const QString name = value.toObject().value(QStringLiteral("file_path")).toString();
                const QString path = QDir(folder.absoluteFilePath()).filePath(name);
                if (name.isEmpty() || !insideDirectory(folder.absoluteFilePath(), path) || !QFileInfo(path).isFile())
                    result->warnings << QStringLiteral("Missing LAS: ") + path;
                else result->clouds.append({path, map.value(QStringLiteral("parent_type")).toString()});
            }
        }
    }
};

class ImageFolderAdapter final : public InputAdapter {
public:
    QString id() const override { return QStringLiteral("IMAGE_FOLDER"); }
    bool detects(const QString &root) const override {
        QDirIterator files(root, {QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.png"),
                                  QStringLiteral("*.JPG"), QStringLiteral("*.JPEG"), QStringLiteral("*.PNG")},
                           QDir::Files, QDirIterator::Subdirectories);
        return files.hasNext();
    }
    void collect(const QString &root, ScanResult *result) const override {
        QDirIterator files(root, {QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.png"),
                                  QStringLiteral("*.JPG"), QStringLiteral("*.JPEG"), QStringLiteral("*.PNG")},
                           QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString path = files.next();
            SourceImage image;
            image.sourcePath = path;
            image.sourceId = QDir(root).relativeFilePath(path);
            if (!imageSize(path, &image.width, &image.height))
                result->warnings << QStringLiteral("Unreadable image: ") + path;
            else result->images.append(image);
        }
        result->warnings << QStringLiteral("Image-folder adapter imports pixels only; flight pose and camera metadata are not interpreted yet.");
    }
};
}

bool InputAdapters::scan(const QString &root, const QString &requestedAdapter,
                         ScanResult *result, QString *error) {
    if (!result || !QDir(root).exists()) {
        if (error) *error = QStringLiteral("Source directory not found.");
        return false;
    }
    EliosAdapter elios;
    ImageFolderAdapter images;
    const InputAdapter *selected = nullptr;
    if (requestedAdapter == QStringLiteral("AUTO")) {
        if (elios.detects(root)) selected = &elios;
        else if (QDir(root).exists(QStringLiteral("snapshots"))) {
            if (error) *error = QStringLiteral("Elios source is incomplete: flights directory is missing.");
            return false;
        }
        else if (images.detects(root)) selected = &images;
    } else if (requestedAdapter == elios.id()) {
        if (elios.detects(root)) selected = &elios;
    } else if (requestedAdapter == images.id() || requestedAdapter == QStringLiteral("DJI_IMAGE_FOLDER")) {
        selected = &images;
    }
    if (!selected) {
        if (error) *error = QStringLiteral("Source format not recognized or does not match the selected adapter.");
        return false;
    }
    *result = {};
    result->adapter = selected->id();
    result->root = QDir(root).absolutePath();
    selected->collect(result->root, result);
    if (result->images.isEmpty()) {
        if (error) *error = QStringLiteral("No usable images found.");
        return false;
    }
    return true;
}
