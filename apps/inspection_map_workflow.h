#pragma once

#include "inspection_map_engine.h"

#include <QList>
#include <QString>

struct ProjectedDamage {
    QString objectId;
    QString imageId;
    QString imageName;
    QString type;
    QString partId;
    QString faceId;
    QString drawingId;
    QString status;
    QString reason;
    QString reviewState;
    QVector<QVector<QVector3D>> paths;
    double lengthMm = 0;
    double areaMm2 = 0;
    double boxWidthMm = 0;
    double boxLengthMm = 0;
    double crackP90Mm = 0;
};

struct ProjectionBatch {
    QString runId;
    QString registrationId;
    QList<ProjectedDamage> items;
    int legacyDimensions = 0;
};

class InspectionMapWorkflow {
public:
    static bool ensureSchema(const QString &projectDb, QString *error);
    static bool run(const QString &projectDb, const QString &inspectionId, const QString &datasetId,
                    const InspectionMapEngine &engine, ProjectionBatch *batch, QString *error);
    static bool active(const QString &projectDb, const QString &inspectionId, const QString &datasetId,
                       ProjectionBatch *batch, QString *error,
                       const InspectionMapEngine *engine = nullptr);
    static bool review(const QString &projectDb, const QString &runId, const QString &objectId,
                       const InspectionMapEngine &engine,
                       const QString &decision, const ProjectedDamage *corrected, QString *error);
};
