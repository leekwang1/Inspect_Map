#pragma once

#include "inspection_map_engine.h"

#include <QList>
#include <QString>
#include <QStringList>

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
    QString groupId;
    QString groupKind;
    QStringList sourceObjectIds;
    QStringList sourceImageNames;
};

struct ProjectionBatch {
    QString runId;
    QString registrationId;
    QList<ProjectedDamage> items;
};

class InspectionMapWorkflow {
public:
    static bool ensureSchema(const QString &projectDb, QString *error);
    static bool run(const QString &projectDb, const QString &inspectionId, const QString &datasetId,
                    const InspectionMapEngine &engine, ProjectionBatch *batch, QString *error);
    static bool active(const QString &projectDb, const QString &inspectionId, const QString &datasetId,
                       ProjectionBatch *batch, QString *error);
    static bool review(const QString &projectDb, const QString &runId, const QString &objectId,
                       const QString &decision, QString *error);
    static bool group(const QString &projectDb, const QString &runId, const QStringList &objectIds,
                      const QString &representativeId, const QString &kind, QString *error);
    static bool ungroup(const QString &projectDb, const QString &runId,
                        const QString &groupId, QString *error);
};
