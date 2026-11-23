#pragma once

#include <QList>
#include <QString>

struct SourceImage {
    QString sourceId;
    QString sourcePath;
    QString flightId;
    QString poseJson;
    int width = 0;
    int height = 0;
    qint64 captureTime = 0;
};

struct SourceCloud {
    QString sourcePath;
    QString scope;
};

struct ScanResult {
    QString adapter;
    QString root;
    QList<SourceImage> images;
    QList<SourceCloud> clouds;
    QStringList warnings;
};

struct ImportedImage {
    QString id;
    QString sourceId;
    QString rawPath;
    QString correctedPath;
    int width = 0;
    int height = 0;
    bool hasPose = false;
    QString poseJson;
    QString displayName;
    qint64 captureTime = 0;
};

struct ImportedDataset {
    QString id;
    QString name;
    QString adapter;
    QString sourceRoot;
    int imageCount = 0;
    int poseCount = 0;
    int cloudCount = 0;
    int correctedCount = 0;
};

class ConversionCore {
public:
    static bool scan(const QString &root, const QString &adapter, ScanResult *result, QString *error);
    static bool importDataset(const QString &projectDb, const QString &inspectionId,
                              const ScanResult &scan, QString *datasetId, QString *error);
    static bool images(const QString &projectDb, const QString &datasetId,
                       QList<ImportedImage> *items, QString *error);
    static bool datasets(const QString &projectDb, QList<ImportedDataset> *items, QString *error);
    static bool clouds(const QString &projectDb, const QString &datasetId,
                       QList<SourceCloud> *items, QString *error);
    static bool activeProfile(const QString &projectDb, const QString &datasetId,
                              QString *path, QString *error);
    static bool registerCorrections(const QString &projectDb, const QString &datasetId,
                                    const QString &profilePath, int *registered, QString *error);
    static bool ensureSchema(const QString &projectDb, QString *error);
    static bool restoreReadableNames(const QString &projectDb, const QString &datasetId, int *restored, QString *error);
    static QString correctedRelativePath(const QString &datasetId, const QString &profileHash, const ImportedImage &image);
};
