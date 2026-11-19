#pragma once

#include <QString>
#include <QStringList>

#include <functional>

struct CorrectionResult {
    int processed = 0;
    int failed = 0;
    QStringList errors;
};

struct CorrectionProfileInfo {
    QString cameraModel;
    QString distortionModel;
    int width = 0;
    int height = 0;
};

class ImageCorrection {
public:
    static bool inspectProfile(const QString &path, CorrectionProfileInfo *info, QString *error);
    static bool run(const QString &projectDb, const QString &datasetId, const QString &profilePath,
                    CorrectionResult *result, const std::function<void(int, int)> &progress,
                    QString *error);
};
