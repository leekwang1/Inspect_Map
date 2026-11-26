#include "inspection_map_engine.h"
#include "inspection_map_workflow.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QDebug>
#include <cstdio>

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    if (argc < 3) return 1;
    const QString sourceDb = QString::fromLocal8Bit(argv[1]);
    const QString modelRoot = QString::fromLocal8Bit(argv[2]);
    QTemporaryDir temp;
    if (!temp.isValid()) return 2;
    const QString dbPath = QDir(temp.path()).filePath(QStringLiteral("project.sqlite"));
    if (!QFile::copy(sourceDb, dbPath)) return 3;
    const QDir calibrationSource(QFileInfo(sourceDb).dir().filePath(QStringLiteral("source/calibration")));
    const QString calibrationTarget = QDir(temp.path()).filePath(QStringLiteral("source/calibration"));
    if (!QDir().mkpath(calibrationTarget)) return 4;
    for (const auto &name : calibrationSource.entryList({QStringLiteral("*.json")}, QDir::Files))
        if (!QFile::copy(calibrationSource.filePath(name), QDir(calibrationTarget).filePath(name))) return 5;
    QString inspectionId, datasetId;
    {
        const QString connection = QStringLiteral("map-workflow-test");
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(dbPath);
            if (!db.open()) return 6;
            QSqlQuery query(db);
            if (!query.exec(QStringLiteral("SELECT inspection_id,dataset_id FROM registration_result "
                    "WHERE active=1 AND review_status='CONFIRMED' LIMIT 1")) || !query.next()) return 7;
            inspectionId = query.value(0).toString(); datasetId = query.value(1).toString();
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }
    InspectionMapEngine engine;
    QString error;
    if (!engine.load(modelRoot, &error)) { qCritical().noquote() << error; return 8; }
    ProjectionBatch first;
    if (!InspectionMapWorkflow::run(dbPath, inspectionId, datasetId, engine, &first, &error)) {
        std::fprintf(stderr, "Projection failed: %s\n", qPrintable(error)); return 9;
    }
    if (first.items.isEmpty()) return 10;
    ProjectionBatch restored;
    if (!InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &restored, &error) ||
        restored.runId != first.runId || restored.items.size() != first.items.size()) return 11;
    int measuredCracks = 0;
    for (const auto &item : first.items) {
        if (item.status != QStringLiteral("PROJECTED") || item.type != QStringLiteral("crack") ||
            item.boxWidthMm <= 0 || item.boxLengthMm <= item.boxWidthMm || item.crackP90Mm <= 0) continue;
        for (const auto &saved : restored.items) if (saved.objectId == item.objectId &&
            qAbs(saved.boxWidthMm-item.boxWidthMm) < 0.001 &&
            qAbs(saved.boxLengthMm-item.boxLengthMm) < 0.001 &&
            qAbs(saved.crackP90Mm-item.crackP90Mm) < 0.001) ++measuredCracks;
    }
    if (measuredCracks == 0) return 22;
    ProjectionBatch second;
    if (!InspectionMapWorkflow::run(dbPath, inspectionId, datasetId, engine, &second, &error) ||
        second.runId == first.runId || second.items.size() != first.items.size()) return 12;
    int projected = 0, review = 0, unavailable = 0;
    for (const auto &item : second.items) {
        if (item.status == QStringLiteral("PROJECTED")) ++projected;
        else if (item.status == QStringLiteral("NEEDS_REVIEW")) ++review;
        else if (item.status == QStringLiteral("UNAVAILABLE")) ++unavailable;
        else return 13;
    }
    std::fprintf(stderr, "Projection: %d projected, %d review, %d unavailable\n",
                 projected, review, unavailable);
    if (unavailable == second.items.size()) return 14;
    const QString objectId = second.items.first().objectId;
    auto reviewed = [&objectId](const ProjectionBatch &batch) -> const ProjectedDamage * {
        for (const auto &item : batch.items) if (item.objectId == objectId) return &item;
        return nullptr;
    };
    if (!InspectionMapWorkflow::review(dbPath, second.runId, objectId, engine,
            QStringLiteral("EXCLUDED"), nullptr, &error)) {
        std::fprintf(stderr, "Review failed: %s\n", qPrintable(error)); return 15;
    }
    ProjectionBatch checked;
    if (!InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &checked, &error) ||
        !reviewed(checked) || reviewed(checked)->status != QStringLiteral("EXCLUDED")) return 16;
    if (!InspectionMapWorkflow::review(dbPath, second.runId, objectId, engine,
            QStringLiteral("DEFERRED"), nullptr, &error) ||
        !InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &checked, &error) ||
        !reviewed(checked) || reviewed(checked)->reviewState != QStringLiteral("DEFERRED")) return 17;
    const auto hit = engine.cast(QVector3D(3, -5, 22.649f), QVector3D(0, 1, 0));
    const auto *drawing = engine.drawingForFace(hit.faceId);
    if (!hit.found || !drawing) return 18;
    QVector3D secondPoint;
    const QPointF flat = InspectionMapEngine::drawingPoint(*drawing, hit.point);
    if (!engine.mapPoint(drawing->id, hit.faceId, flat + QPointF(0.1, 0), &secondPoint)) return 19;
    ProjectedDamage corrected = second.items.first();
    corrected.partId = hit.partId; corrected.faceId = hit.faceId; corrected.drawingId = drawing->id;
    corrected.paths = {{hit.point, secondPoint}};
    corrected.lengthMm = 1000.0 * double((secondPoint-hit.point).length());
    corrected.areaMm2 = 0;
    if (!InspectionMapWorkflow::review(dbPath, second.runId, objectId, engine,
            QStringLiteral("CORRECTED"), &corrected, &error) ||
        !InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &checked, &error) ||
        !reviewed(checked) || reviewed(checked)->status != QStringLiteral("PROJECTED") ||
        reviewed(checked)->reviewState != QStringLiteral("CORRECTED") ||
        reviewed(checked)->lengthMm <= 0) return 20;
    if (!InspectionMapWorkflow::review(dbPath, second.runId, objectId, engine,
            QStringLiteral("RESET"), nullptr, &error) ||
        !InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &checked, &error) ||
        !reviewed(checked) || !reviewed(checked)->reviewState.isEmpty() ||
        reviewed(checked)->status != second.items.first().status) return 21;
    return 0;
}
