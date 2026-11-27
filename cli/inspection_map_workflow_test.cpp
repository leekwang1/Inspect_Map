#include "inspection_map_engine.h"
#include "inspection_map_workflow.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlRecord>
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
    const QString freshDb = QDir(temp.path()).filePath(QStringLiteral("fresh.sqlite"));
    QString schemaError;
    if (!InspectionMapWorkflow::ensureSchema(freshDb, &schemaError)) return 36;
    {
        const QString connection = QStringLiteral("map-schema-test");
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(freshDb);
            if (!db.open()) return 37;
            const auto columns = db.record(QStringLiteral("map_projection_review"));
            const QStringList expected{QStringLiteral("run_id"), QStringLiteral("object_id"),
                QStringLiteral("decision"), QStringLiteral("updated_at")};
            if (columns.count() != expected.size()) return 38;
            for (int i = 0; i < columns.count(); ++i)
                if (columns.fieldName(i) != expected[i]) return 39;
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }
    ProjectionBatch exportBatch;
    exportBatch.runId = QStringLiteral("run-1");
    exportBatch.registrationId = QStringLiteral("registration-1");
    ProjectedDamage included;
    included.objectId = QStringLiteral("damage-1");
    included.imageName = QStringLiteral("image-1.jpg");
    included.type = QStringLiteral("crack");
    included.partId = QStringLiteral("part-1");
    included.faceId = QStringLiteral("face-1");
    included.status = QStringLiteral("PROJECTED");
    included.paths = {{QVector3D(1, 2, 3), QVector3D(4, 5, 6)}};
    included.boxWidthMm = 400;
    included.boxLengthMm = 4000;
    included.crackP90Mm = 0.3;
    included.groupId = QStringLiteral("group-1");
    included.groupKind = QStringLiteral("CONNECTED");
    included.sourceObjectIds = {QStringLiteral("damage-1"), QStringLiteral("damage-2")};
    included.sourceImageNames = {QStringLiteral("image-1.jpg"), QStringLiteral("image-2.jpg")};
    exportBatch.items.append(included);
    auto omitted = included;
    omitted.objectId = QStringLiteral("damage-3");
    omitted.status = QStringLiteral("EXCLUDED");
    exportBatch.items.append(omitted);
    omitted.objectId = QStringLiteral("damage-4");
    omitted.status = QStringLiteral("PROJECTED");
    omitted.reviewState = QStringLiteral("DEFERRED");
    exportBatch.items.append(omitted);
    omitted.objectId = QStringLiteral("damage-5");
    omitted.status = QStringLiteral("NEEDS_REVIEW");
    omitted.reviewState.clear();
    exportBatch.items.append(omitted);
    const QString exportPath = QDir(temp.path()).filePath(QStringLiteral("model_damage_3d.json"));
    int exported = 0;
    QString exportError;
    if (!InspectionMapWorkflow::exportModelDamageJson(exportBatch, QStringLiteral("inspection-1"),
            QStringLiteral("dataset-1"), exportPath, &exported, &exportError) || exported != 1) return 40;
    QFile exportFile(exportPath);
    if (!exportFile.open(QIODevice::ReadOnly)) return 41;
    const auto document = QJsonDocument::fromJson(exportFile.readAll()).object();
    const auto damages = document.value(QStringLiteral("damages")).toArray();
    const auto damage = damages.isEmpty() ? QJsonObject() : damages.first().toObject();
    const auto geometry = damage.value(QStringLiteral("geometry")).toObject();
    const auto coordinates = geometry.value(QStringLiteral("paths_xyz")).toArray();
    const auto measurements = damage.value(QStringLiteral("measurements")).toObject();
    const auto provenance = damage.value(QStringLiteral("source")).toObject();
    const auto omittedCounts = document.value(QStringLiteral("omitted")).toObject();
    if (document.value(QStringLiteral("schema")).toString() != QStringLiteral("inspectmap.model_damage.v1") ||
        document.value(QStringLiteral("coordinate_system")).toObject().value(QStringLiteral("unit")).toString() != QStringLiteral("m") ||
        document.value(QStringLiteral("damage_count")).toInt() != 1 || damages.size() != 1 ||
        damage.value(QStringLiteral("damage_id")).toString() != included.objectId ||
        geometry.value(QStringLiteral("type")).toString() != QStringLiteral("MultiPath3D") ||
        coordinates.size() != 1 || coordinates.first().toArray().size() != 2 ||
        coordinates.first().toArray().first().toArray().first().toDouble() != 1 ||
        measurements.value(QStringLiteral("bbox_width_m")).toDouble() != 0.4 ||
        measurements.value(QStringLiteral("bbox_length_m")).toDouble() != 4.0 ||
        measurements.value(QStringLiteral("crack_width_p90_mm")).toDouble() != 0.3 ||
        provenance.value(QStringLiteral("object_ids")).toArray().size() != 2 ||
        omittedCounts.value(QStringLiteral("excluded")).toInt() != 1 ||
        omittedCounts.value(QStringLiteral("deferred")).toInt() != 1 ||
        omittedCounts.value(QStringLiteral("unprojected")).toInt() != 1) return 42;
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
    if (!InspectionMapWorkflow::review(dbPath, second.runId, objectId,
            QStringLiteral("EXCLUDED"), &error)) {
        std::fprintf(stderr, "Review failed: %s\n", qPrintable(error)); return 15;
    }
    ProjectionBatch checked;
    if (!InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &checked, &error) ||
        !reviewed(checked) || reviewed(checked)->status != QStringLiteral("EXCLUDED")) return 16;
    if (!InspectionMapWorkflow::review(dbPath, second.runId, objectId,
            QStringLiteral("DEFERRED"), &error) ||
        !InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &checked, &error) ||
        !reviewed(checked) || reviewed(checked)->reviewState != QStringLiteral("DEFERRED")) return 17;
    if (InspectionMapWorkflow::review(dbPath, second.runId, objectId,
            QStringLiteral("CORRECTED"), &error)) return 18;
    if (!InspectionMapWorkflow::review(dbPath, second.runId, objectId,
            QStringLiteral("RESET"), &error) ||
        !InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &checked, &error) ||
        !reviewed(checked) || !reviewed(checked)->reviewState.isEmpty() ||
        reviewed(checked)->status != second.items.first().status) return 21;
    QStringList crackIds;
    const ProjectedDamage *firstCrack = nullptr, *secondCrack = nullptr;
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
            firstCrack = &item; secondCrack = &*next;
            crackIds = {item.objectId, next->objectId}; break;
        }
    }
    if (crackIds.size() != 2) return 23;
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
    if (InspectionMapWorkflow::review(dbPath, second.runId, crackIds.first(),
            QStringLiteral("RESET"), &error) ||
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
        representative->paths.size() != firstCrack->paths.size() + secondCrack->paths.size() ||
        qAbs(representative->lengthMm - firstCrack->lengthMm - secondCrack->lengthMm) > 0.001 ||
        qAbs(representative->boxLengthMm - firstCrack->boxLengthMm - secondCrack->boxLengthMm) > 0.001) return 29;
    if (!InspectionMapWorkflow::ungroup(dbPath, second.runId, representative->groupId, &error) ||
        !InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &checked, &error) ||
        checked.items.size() != second.items.size()) return 30;
    return 0;
}
