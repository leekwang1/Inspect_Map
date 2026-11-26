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
#include <algorithm>
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
    QStringList crackIds;
    for (const auto &item : second.items)
        if (item.type == QStringLiteral("crack") && crackIds.size() < 2) crackIds.append(item.objectId);
    if (crackIds.size() != 2) return 23;
    for (const auto &id : crackIds) {
        const auto source = std::find_if(second.items.cbegin(), second.items.cend(),
            [&](const auto &item) { return item.objectId == id; });
        ProjectedDamage trace = *source;
        trace.partId = hit.partId; trace.faceId = hit.faceId; trace.drawingId = drawing->id;
        trace.paths = {{hit.point, secondPoint}};
        trace.lengthMm = corrected.lengthMm; trace.areaMm2 = 0;
        if (!InspectionMapWorkflow::review(dbPath, second.runId, id, engine,
                QStringLiteral("CORRECTED"), &trace, &error)) return 24;
    }
    if (!InspectionMapWorkflow::group(dbPath, second.runId, crackIds, crackIds.first(),
            QStringLiteral("DUPLICATE"), &error) ||
        !InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &checked, &error) ||
        checked.items.size() != second.items.size()-1) return 25;
    auto representative = std::find_if(checked.items.cbegin(), checked.items.cend(),
        [&](const auto &item) { return item.objectId == crackIds.first(); });
    if (representative == checked.items.cend() ||
        representative->groupKind != QStringLiteral("DUPLICATE") ||
        representative->sourceObjectIds.size() != 2) return 26;
    const QString duplicateGroup = representative->groupId;
    if (InspectionMapWorkflow::review(dbPath, second.runId, crackIds.first(), engine,
            QStringLiteral("RESET"), nullptr, &error) ||
        !InspectionMapWorkflow::ungroup(dbPath, second.runId, duplicateGroup, &error) ||
        !InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &checked, &error) ||
        checked.items.size() != second.items.size()) return 27;
    if (!InspectionMapWorkflow::group(dbPath, second.runId, crackIds, crackIds.first(),
            QStringLiteral("CONNECTED"), &error) ||
        !InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &checked, &error) ||
        checked.items.size() != second.items.size()-1) return 28;
    representative = std::find_if(checked.items.cbegin(), checked.items.cend(),
        [&](const auto &item) { return item.objectId == crackIds.first(); });
    if (representative == checked.items.cend() ||
        representative->groupKind != QStringLiteral("CONNECTED") ||
        representative->paths.size() != 2 ||
        qAbs(representative->lengthMm - 2 * corrected.lengthMm) > 0.001) return 29;
    if (!InspectionMapWorkflow::ungroup(dbPath, second.runId, representative->groupId, &error) ||
        !InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &checked, &error) ||
        checked.items.size() != second.items.size()) return 30;
    QStringList nativeIds;
    const ProjectedDamage *nativeFirst = nullptr, *nativeSecond = nullptr;
    for (const auto &item : second.items) {
        if (item.type != QStringLiteral("crack") || item.status != QStringLiteral("PROJECTED") ||
            item.paths.isEmpty() || item.boxLengthMm <= 0) continue;
        const auto next = std::find_if(second.items.cbegin(), second.items.cend(), [&](const auto &candidate) {
            return candidate.objectId != item.objectId && candidate.type == item.type &&
                candidate.status == QStringLiteral("PROJECTED") && !candidate.paths.isEmpty() &&
                candidate.partId == item.partId && candidate.faceId == item.faceId &&
                candidate.boxLengthMm > 0;
        });
        if (next != second.items.cend()) {
            nativeFirst = &item; nativeSecond = &*next;
            nativeIds = {item.objectId, next->objectId}; break;
        }
    }
    if (nativeIds.size() != 2) return 31;
    for (const auto &id : nativeIds)
        if (!InspectionMapWorkflow::review(dbPath, second.runId, id, engine,
                QStringLiteral("RESET"), nullptr, &error)) return 32;
    if (!InspectionMapWorkflow::group(dbPath, second.runId, nativeIds, nativeIds.first(),
            QStringLiteral("CONNECTED"), &error) ||
        !InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &checked, &error)) return 33;
    representative = std::find_if(checked.items.cbegin(), checked.items.cend(),
        [&](const auto &item) { return item.objectId == nativeIds.first(); });
    if (representative == checked.items.cend() || !representative->reviewState.isEmpty() ||
        representative->paths.size() != nativeFirst->paths.size() + nativeSecond->paths.size() ||
        qAbs(representative->lengthMm - nativeFirst->lengthMm - nativeSecond->lengthMm) > 0.001 ||
        qAbs(representative->boxLengthMm - nativeFirst->boxLengthMm - nativeSecond->boxLengthMm) > 0.001 ||
        qAbs(representative->boxWidthMm - std::max(nativeFirst->boxWidthMm, nativeSecond->boxWidthMm)) > 0.001)
        return 34;
    if (!InspectionMapWorkflow::ungroup(dbPath, second.runId, representative->groupId, &error) ||
        !InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &checked, &error) ||
        checked.items.size() != second.items.size()) return 35;
    return 0;
}
