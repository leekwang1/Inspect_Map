#include "project_store.h"
#include "project_archive.h"
#include "conversion_core.h"
#include "image_correction.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QTextStream out(stdout), err(stderr);
    const auto args = app.arguments();
    QString error;
    if (args.size() == 3 && args[1] == QStringLiteral("create")) {
        const QString path = QDir(args[2]).absolutePath();
        if (!ProjectStore::create(path, QFileInfo(path).fileName(), &error)) {
            err << error << Qt::endl;
            return 1;
        }
        out << ProjectStore::databasePath(path) << Qt::endl;
        return 0;
    }
    if (args.size() == 3 && args[1] == QStringLiteral("info")) {
        ProjectInfo info;
        if (!ProjectStore::read(args[2], &info, &error)) {
            err << error << Qt::endl;
            return 1;
        }
        out << info.id << "\t" << info.name << "\t" << info.createdAt << Qt::endl;
        return 0;
    }
    if (args.size() == 5 && args[1] == QStringLiteral("add-facility")) {
        if (!ProjectStore::addFacility(args[2], args[3], args[4], &error)) {
            err << error << Qt::endl;
            return 1;
        }
        return 0;
    }
    if (args.size() == 6 && args[1] == QStringLiteral("add-inspection")) {
        if (!ProjectStore::addInspection(args[2], args[3], args[4], args[5], &error)) {
            err << error << Qt::endl;
            return 1;
        }
        return 0;
    }
    if (args.size() == 4 && args[1] == QStringLiteral("convert-scan")) {
        ScanResult scan;
        if (!ConversionCore::scan(args[2], args[3], &scan, &error)) {
            err << error << Qt::endl;
            return 1;
        }
        out << scan.adapter << "\timages=" << scan.images.size() << "\tclouds=" << scan.clouds.size()
            << "\twarnings=" << scan.warnings.size() << Qt::endl;
        for (const auto &warning : scan.warnings) err << warning << Qt::endl;
        return 0;
    }
    if (args.size() == 6 && args[1] == QStringLiteral("convert-import")) {
        ScanResult scan;
        if (!ConversionCore::scan(args[4], args[5], &scan, &error)) {
            err << error << Qt::endl;
            return 1;
        }
        QString datasetId;
        const QString inspectionId = args[3] == QStringLiteral("-") ? QString() : args[3];
        if (!ConversionCore::importDataset(args[2], inspectionId, scan, &datasetId, &error)) {
            err << error << Qt::endl;
            return 1;
        }
        out << datasetId << "\timages=" << scan.images.size() << "\tclouds=" << scan.clouds.size() << Qt::endl;
        return 0;
    }
    if (args.size() == 5 && args[1] == QStringLiteral("convert-correct")) {
        CorrectionResult result;
        if (!ImageCorrection::run(args[2], args[3], args[4], &result, {}, &error)) {
            err << error << Qt::endl;
            return 1;
        }
        int registered = 0;
        if (!ConversionCore::registerCorrections(args[2], args[3], args[4], &registered, &error)) {
            err << error << Qt::endl;
            return 1;
        }
        out << "processed=" << result.processed << " failed=" << result.failed << " registered=" << registered << Qt::endl;
        for (const auto &item : result.errors) err << item << Qt::endl;
        return result.failed ? 1 : 0;
    }
    if (args.size() == 2 && args[1] == QStringLiteral("converter-self-test")) {
        QTemporaryDir temp;
        const QString project = temp.filePath(QStringLiteral("project"));
        const QString elios = temp.filePath(QStringLiteral("elios"));
        const QString snapshot = QDir(elios).filePath(QStringLiteral("snapshots/snapshot-1"));
        const QString map = QDir(elios).filePath(QStringLiteral("maps/map-1"));
        const QString dji = temp.filePath(QStringLiteral("dji"));
        if (!QDir().mkpath(project) || !QDir().mkpath(snapshot) || !QDir().mkpath(map) ||
            !QDir().mkpath(QDir(elios).filePath(QStringLiteral("flights"))) || !QDir().mkpath(dji)) return 1;
        QImage testImage(40, 30, QImage::Format_RGB32);
        testImage.fill(Qt::red);
        if (!testImage.save(QDir(snapshot).filePath(QStringLiteral("image.jpg"))) ||
            !testImage.save(QDir(dji).filePath(QStringLiteral("DJI_0001.jpg")))) return 1;
        QFile snapshotFile(QDir(snapshot).filePath(QStringLiteral("snapshot.json")));
        if (!snapshotFile.open(QIODevice::WriteOnly)) return 1;
        snapshotFile.write(QJsonDocument(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("snapshot-1")},
            {QStringLiteral("flight_id"), QStringLiteral("flight-1")},
            {QStringLiteral("camera_image"), QStringLiteral("image.jpg")},
            {QStringLiteral("camera_pose"), QJsonObject{
                {QStringLiteral("position"), QJsonObject{{QStringLiteral("x"), 1}, {QStringLiteral("y"), 2}, {QStringLiteral("z"), 3}}}}}
        }).toJson());
        snapshotFile.close();
        QFile mapFile(QDir(map).filePath(QStringLiteral("map.json")));
        QFile lasFile(QDir(map).filePath(QStringLiteral("map.las")));
        if (!mapFile.open(QIODevice::WriteOnly) || !lasFile.open(QIODevice::WriteOnly)) return 1;
        lasFile.write("LASF"); lasFile.close();
        mapFile.write(QJsonDocument(QJsonObject{
            {QStringLiteral("parent_type"), QStringLiteral("Flight")},
            {QStringLiteral("point_clouds"), QJsonArray{QJsonObject{{QStringLiteral("file_path"), QStringLiteral("map.las")}}}}
        }).toJson());
        mapFile.close();
        ScanResult eliosScan, djiScan, legacyScan;
        if (!ProjectStore::create(project, QStringLiteral("Test"), &error) ||
            !ConversionCore::scan(elios, QStringLiteral("AUTO"), &eliosScan, &error) ||
            !ConversionCore::scan(dji, QStringLiteral("AUTO"), &djiScan, &error) ||
            !ConversionCore::scan(dji, QStringLiteral("DJI_IMAGE_FOLDER"), &legacyScan, &error) ||
            eliosScan.adapter != QStringLiteral("ELIOS_INSPECTOR5") ||
            eliosScan.images.size() != 1 || eliosScan.clouds.size() != 1 ||
            djiScan.adapter != QStringLiteral("IMAGE_FOLDER") || legacyScan.adapter != djiScan.adapter ||
            djiScan.images.size() != 1 || !djiScan.images[0].poseJson.isEmpty() || !djiScan.clouds.isEmpty()) {
            err << "converter scan failed: " << error << Qt::endl;
            return 1;
        }
        const QString db = ProjectStore::databasePath(project);
        QString firstId, secondId, imageFolderId;
        QList<ImportedImage> imported;
        QList<ImportedDataset> datasets;
        if (!ConversionCore::importDataset(db, {}, eliosScan, &firstId, &error) ||
            !ConversionCore::importDataset(db, {}, eliosScan, &secondId, &error) ||
            firstId != secondId || !ConversionCore::datasets(db, &datasets, &error) ||
            !ConversionCore::images(db, firstId, &imported, &error) ||
            datasets.size() != 1 || datasets[0].poseCount != 1 || datasets[0].cloudCount != 1 ||
            imported.size() != 1 || !imported[0].hasPose ||
            imported[0].displayName != QStringLiteral("image.jpg") ||
            QFileInfo(imported[0].rawPath).fileName() != QStringLiteral("image.jpg") ||
            !QFileInfo(QDir(project).filePath(imported[0].rawPath)).isFile() ||
            !ProjectStore::ensureManagerSchema(db, &error)) {
            err << "converter import failed: " << error << Qt::endl;
            return 1;
        }
        if (!ConversionCore::importDataset(db, {}, djiScan, &imageFolderId, &error) ||
            !ConversionCore::datasets(db, &datasets, &error) || datasets.size() != 2) {
            err << "image-folder import failed: " << error << Qt::endl;
            return 1;
        }
        bool folderCapabilities = false;
        for (const auto &dataset : datasets)
            if (dataset.id == imageFolderId) folderCapabilities = dataset.imageCount == 1 &&
                dataset.poseCount == 0 && dataset.cloudCount == 0 && dataset.correctedCount == 0;
        if (!folderCapabilities) { err << "image-folder capabilities incorrect" << Qt::endl; return 1; }
        const QByteArray profileBytes = R"({"distortion_model":"opencv_fisheye_kannala_brandt","image_size":{"width":40,"height":30},"K":[[20,0,20],[0,20,15],[0,0,1]],"D":[0,0,0,0],"P":[[20,0,20],[0,20,15],[0,0,1]]})";
        const QString hash = QString::fromLatin1(QCryptographicHash::hash(profileBytes, QCryptographicHash::Sha256).toHex());
        const QString profilePath = temp.filePath(QStringLiteral("profile.json"));
        QFile profile(profilePath);
        if (!profile.open(QIODevice::WriteOnly) || profile.write(profileBytes) != profileBytes.size()) return 1;
        profile.close();
        CorrectionProfileInfo profileInfo;
        if (!ImageCorrection::inspectProfile(profilePath, &profileInfo, &error) ||
            profileInfo.width != 40 || profileInfo.height != 30) {
            err << "profile inspection failed: " << error << Qt::endl;
            return 1;
        }
        const QString correctedDir = QDir(project).filePath(
            QStringLiteral("derived/images/device_undistorted/") + firstId + QStringLiteral("/") + hash);
        CorrectionResult correction;
        if (!ImageCorrection::run(db, firstId, profilePath, &correction, {}, &error) ||
            correction.processed != 1 || correction.failed != 0 ||
            !QFileInfo(QDir(project).filePath(ConversionCore::correctedRelativePath(firstId, hash, imported[0]))).isFile()) {
            err << "converter correction failed: " << error << " " << correction.errors.join(" | ") << Qt::endl;
            return 1;
        }
        int registered = 0;
        if (!ConversionCore::registerCorrections(db, firstId, profilePath, &registered, &error) ||
            registered != 1 || !ConversionCore::images(db, firstId, &imported, &error) ||
            imported[0].correctedPath.isEmpty()) {
            err << "converter correction registration failed: " << error << Qt::endl;
            return 1;
        }
        const QString oldRaw = QStringLiteral("source/images/") + firstId + QStringLiteral("/") + imported[0].id + QStringLiteral(".jpg");
        const QString oldCorrected = QStringLiteral("derived/images/device_undistorted/") + firstId +
            QStringLiteral("/") + hash + QStringLiteral("/") + imported[0].id + QStringLiteral(".jpg");
        if (!QFile::rename(QDir(project).filePath(imported[0].rawPath), QDir(project).filePath(oldRaw)) ||
            !QFile::rename(QDir(project).filePath(imported[0].correctedPath), QDir(project).filePath(oldCorrected))) return 1;
        {
            auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("legacy-image-test"));
            database.setDatabaseName(db);
            if (!database.open()) return 1;
            QSqlQuery query(database);
            query.prepare(QStringLiteral("UPDATE image SET raw_path=?,display_name=NULL,capture_time=NULL WHERE image_id=?"));
            query.addBindValue(oldRaw); query.addBindValue(imported[0].id);
            if (!query.exec()) return 1;
            query.prepare(QStringLiteral("UPDATE image_variant SET storage_path=? WHERE image_id=? AND variant_type='RAW'"));
            query.addBindValue(oldRaw); query.addBindValue(imported[0].id);
            if (!query.exec()) return 1;
            query.prepare(QStringLiteral("UPDATE image_variant SET storage_path=? WHERE image_id=? AND variant_type='DEVICE_UNDISTORTED'"));
            query.addBindValue(oldCorrected); query.addBindValue(imported[0].id);
            if (!query.exec()) return 1;
            database.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("legacy-image-test"));
        int restoredCount = 0;
        if (!ConversionCore::restoreReadableNames(db, firstId, &restoredCount, &error) || restoredCount != 1 ||
            !ConversionCore::images(db, firstId, &imported, &error) ||
            imported[0].displayName != QStringLiteral("image.jpg") ||
            QFileInfo(imported[0].rawPath).fileName() != QStringLiteral("image.jpg") ||
            QFileInfo(imported[0].correctedPath).fileName() != QStringLiteral("image.jpg") ||
            !QFileInfo::exists(QDir(project).filePath(imported[0].rawPath)) ||
            !QFileInfo::exists(QDir(project).filePath(imported[0].correctedPath)) ||
            !QFileInfo::exists(QDir(project).filePath(oldRaw))) {
            err << "legacy image name restoration failed: " << error << Qt::endl;
            return 1;
        }
        QString activeProfile;
        CorrectionResult folderCorrection;
        if (!ConversionCore::activeProfile(db, firstId, &activeProfile, &error) ||
            !QFileInfo(activeProfile).isFile() ||
            !ImageCorrection::run(db, imageFolderId, profilePath, &folderCorrection, {}, &error) ||
            folderCorrection.processed != 1 || folderCorrection.failed != 0 ||
            !ConversionCore::registerCorrections(db, imageFolderId, profilePath, &registered, &error) ||
            registered != 1 || !ConversionCore::images(db, imageFolderId, &imported, &error) ||
            imported.size() != 1 || imported[0].hasPose || imported[0].correctedPath.isEmpty() ||
            !ConversionCore::datasets(db, &datasets, &error)) {
            err << "independent image-folder correction failed: " << error << Qt::endl;
            return 1;
        }
        bool correctedFolder = false;
        for (const auto &dataset : datasets)
            if (dataset.id == imageFolderId) correctedFolder = dataset.correctedCount == 1;
        if (!correctedFolder) { err << "corrected image count incorrect" << Qt::endl; return 1; }
        out << "converter self-test passed" << Qt::endl;
        return 0;
    }
    if (args.size() == 2 && args[1] == QStringLiteral("self-test")) {
        QTemporaryDir quick;
        const QString quickModel = quick.filePath(QStringLiteral("model"));
        const QString quickLas = quick.filePath(QStringLiteral("survey.las"));
        QFile quickCloud(quickLas);
        QString quickModelRead, quickCloudRead;
        QList<FacilityInfo> quickFacilities;
        QList<InspectionInfo> quickInspections;
        if (!quick.isValid() || !QDir().mkpath(quickModel) || !quickCloud.open(QIODevice::WriteOnly)) return 1;
        quickCloud.close();
        const QString quickProjectFolder = quick.filePath(QStringLiteral("project"));
        if (!QDir().mkpath(quickProjectFolder)
            || !ProjectStore::create(quickProjectFolder, QStringLiteral("Quick"),
                ProjectSetup{QStringLiteral("Bridge"), QStringLiteral("Visit"), quickModel, quickLas}, &error)
            || !ProjectStore::facilities(ProjectStore::databasePath(quickProjectFolder), &quickFacilities, &error)
            || quickFacilities.size() != 1
            || !ProjectStore::inspections(ProjectStore::databasePath(quickProjectFolder), quickFacilities[0].id, &quickInspections, &error)
            || quickInspections.size() != 1
            || !ProjectStore::facilityModelPath(ProjectStore::databasePath(quickProjectFolder), quickFacilities[0].id, &quickModelRead, &error)
            || !ProjectStore::inspectionCloudPath(ProjectStore::databasePath(quickProjectFolder), quickInspections[0].id, &quickCloudRead, &error)
            || quickModelRead != QFileInfo(quickModel).canonicalFilePath()
            || quickCloudRead != QFileInfo(quickLas).canonicalFilePath()
            || !ProjectStore::setInspectionCloudPath(ProjectStore::databasePath(quickProjectFolder), quickInspections[0].id, {}, &error)
            || !ProjectStore::inspectionCloudPath(ProjectStore::databasePath(quickProjectFolder), quickInspections[0].id, &quickCloudRead, &error)
            || !quickCloudRead.isEmpty()) {
            err << "quick setup failed: " << error << Qt::endl;
            return 1;
        }
        QTemporaryDir temp;
        ProjectInfo info;
        QList<FacilityInfo> facilities;
        QList<InspectionInfo> inspections;
        const auto path = ProjectStore::databasePath(temp.path());
        const auto backupPath = temp.filePath(QStringLiteral("backup.sqlite"));
        if (!temp.isValid() || !ProjectStore::create(temp.path(), QStringLiteral("Smoke Test"), &error)
            || !ProjectStore::read(path, &info, &error)
            || info.name != QStringLiteral("Smoke Test")
            || !ProjectStore::addFacility(path, QStringLiteral("Bridge A"), QStringLiteral("Bridge"), &error)
            || !ProjectStore::facilities(path, &facilities, &error)
            || facilities.size() != 1 || facilities[0].name != QStringLiteral("Bridge A")
            || !ProjectStore::addInspection(path, facilities[0].id, QStringLiteral("Survey 1"), QStringLiteral("Visual"), &error)
            || !ProjectStore::inspections(path, facilities[0].id, &inspections, &error)
            || inspections.size() != 1 || inspections[0].name != QStringLiteral("Survey 1")) {
            err << "self-test failed: " << error << Qt::endl;
            return 1;
        }
        auto updatedFacility = facilities[0];
        updatedFacility.address = QStringLiteral("Seoul");
        auto updatedInspection = inspections[0];
        updatedInspection.startedAt = QStringLiteral("2026-01-01");
        updatedInspection.organization = QStringLiteral("Team A");
        if (!ProjectStore::updateFacility(path, facilities[0], updatedFacility, &error)
            || ProjectStore::updateFacility(path, facilities[0], updatedFacility, &error)
            || !ProjectStore::updateInspection(path, inspections[0], updatedInspection, &error)
            || !ProjectStore::ensureManagerSchema(path, &error)
            || !ProjectStore::setPipelineMode(path, QStringLiteral("sfm"), QStringLiteral("SKIP"), &error)
            || !ProjectStore::backup(path, backupPath, &error)
            || !ProjectStore::read(backupPath, &info, &error)) {
            err << "self-test edit/backup failed: " << error << Qt::endl;
            return 1;
        }
        FacilityInfo detailedFacility{{}, QStringLiteral("Bridge B"), QStringLiteral("Bridge"),
            QStringLiteral("Busan"), QStringLiteral("B-2"), QStringLiteral("Second span")};
        if (!ProjectStore::addFacility(path, detailedFacility, &error)
            || !ProjectStore::facilities(path, &facilities, &error)) return 1;
        QString detailedFacilityId;
        for (const auto &facility : facilities)
            if (facility.name == detailedFacility.name && facility.address == detailedFacility.address &&
                facility.managementCode == detailedFacility.managementCode) detailedFacilityId = facility.id;
        InspectionInfo detailedInspection{{}, detailedFacilityId, QStringLiteral("Survey B"), QStringLiteral("Visual"), {},
            QStringLiteral("2026-01-01"), QStringLiteral("2026-01-02"), QStringLiteral("Team B"), QStringLiteral("Inspector")};
        if (detailedFacilityId.isEmpty() || !ProjectStore::addInspection(path, detailedInspection, &error)
            || !ProjectStore::inspections(path, detailedFacilityId, &inspections, &error)
            || inspections.size() != 1 || inspections[0].organization != detailedInspection.organization ||
            inspections[0].startedAt != detailedInspection.startedAt) {
            err << "detailed create failed: " << error << Qt::endl;
            return 1;
        }
        QMap<QString, QString> modes;
        QString integrity;
        if (!ProjectStore::pipelineModes(path, &modes, &error)
            || modes.value(QStringLiteral("sfm")) != QStringLiteral("SKIP")
            || !ProjectStore::integrityCheck(path, &integrity, &error)) {
            err << "self-test pipeline/integrity failed: " << error << " " << integrity << Qt::endl;
            return 1;
        }
        const auto firstRunId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto secondRunId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto firstArtifactId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto secondArtifactId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto childArtifactId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QFile fixture(temp.filePath(QStringLiteral("fixture.txt")));
        if (!fixture.open(QIODevice::WriteOnly) || fixture.write("test") != 4) return 1;
        fixture.close();
        const auto now = QStringLiteral("2026-01-01T00:00:00Z");
        QList<ModuleRunInfo> runs;
        QList<ArtifactInfo> artifacts;
        if (!ProjectStore::recordRun(path, {firstRunId, facilities[0].id, QStringLiteral("input_conversion"), QStringLiteral("VALID"), now, now, {}}, &error)) {
            // A run is scoped to an inspection, not a facility.
            error.clear();
        }
        if (!ProjectStore::recordRun(path, {secondRunId, inspections[0].id, QStringLiteral("input_conversion"), QStringLiteral("VALID"), now, now, {}}, &error)
            || !ProjectStore::recordArtifact(path, {firstArtifactId, secondRunId, QStringLiteral("TEST"), QStringLiteral("fixture.txt"), QStringLiteral("VALID"), now, false}, &error)
            || !ProjectStore::adoptArtifact(path, firstArtifactId, &error)
            || !ProjectStore::recordArtifact(path, {childArtifactId, secondRunId, QStringLiteral("CHILD"), QStringLiteral("fixture.txt"), QStringLiteral("VALID"), now, false}, &error)
            || !ProjectStore::linkArtifacts(path, firstArtifactId, childArtifactId, &error)
            || !ProjectStore::recordRun(path, {firstRunId, inspections[0].id, QStringLiteral("input_conversion"), QStringLiteral("VALID"), now, now, {}}, &error)
            || !ProjectStore::recordArtifact(path, {secondArtifactId, firstRunId, QStringLiteral("TEST"), QStringLiteral("fixture.txt"), QStringLiteral("VALID"), now, false}, &error)
            || !ProjectStore::adoptArtifact(path, secondArtifactId, &error)
            || !ProjectStore::runs(path, inspections[0].id, &runs, &error)
            || !ProjectStore::artifacts(path, &artifacts, &error)
            || runs.size() != 2 || artifacts.size() != 3) {
            err << "run/artifact test failed: " << error << Qt::endl;
            return 1;
        }
        bool oldInactive = false, newActive = false, childStale = false;
        for (const auto &artifact : artifacts) {
            if (artifact.id == firstArtifactId) oldInactive = !artifact.active;
            if (artifact.id == secondArtifactId) newActive = artifact.active;
            if (artifact.id == childArtifactId) childStale = artifact.status == QStringLiteral("STALE");
        }
        if (!oldInactive || !newActive || !childStale) {
            err << "adoption/dependency test failed" << Qt::endl;
            return 1;
        }
        QTemporaryDir archive;
        QTemporaryDir restored;
        ProjectInfo restoredInfo;
        if (!archive.isValid() || !restored.isValid()
            || !ProjectArchive::backupProject(path, archive.path(), &error)
            || !ProjectArchive::restoreProject(archive.path(), restored.path(), &error)
            || !ProjectStore::read(ProjectStore::databasePath(restored.path()), &restoredInfo, &error)
            || restoredInfo.id != info.id || !QFileInfo::exists(restored.filePath(QStringLiteral("fixture.txt")))) {
            err << "project archive test failed: " << error << Qt::endl;
            return 1;
        }
        QFile tamper(archive.filePath(QStringLiteral("fixture.txt")));
        QTemporaryDir rejected;
        if (!tamper.open(QIODevice::Append) || tamper.write("changed") != 7) return 1;
        tamper.close();
        error.clear();
        if (ProjectArchive::restoreProject(archive.path(), rejected.path(), &error) ||
            !QDir(rejected.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
            err << "tampered archive was accepted" << Qt::endl;
            return 1;
        }
        QTemporaryDir legacy;
        const auto legacyPath = ProjectStore::databasePath(legacy.path());
        if (!legacy.isValid() || !ProjectStore::create(legacy.path(), QStringLiteral("Legacy"), &error)) {
            err << "legacy fixture failed: " << error << Qt::endl;
            return 1;
        }
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("legacy-test"));
            db.setDatabaseName(legacyPath);
            if (!db.open()) return 1;
            QSqlQuery query(db);
            if (!query.exec(QStringLiteral("DROP TABLE artifact_dependency"))
                || !query.exec(QStringLiteral("DROP TABLE inspection_input"))
                || !query.exec(QStringLiteral("DROP TABLE facility_input"))
                || !query.exec(QStringLiteral("DROP TABLE artifact"))
                || !query.exec(QStringLiteral("DROP TABLE module_run"))
                || !query.exec(QStringLiteral("DROP TABLE pipeline_setting"))
                || !query.exec(QStringLiteral("DELETE FROM schema_version"))
                || !query.exec(QStringLiteral("INSERT INTO schema_version VALUES(1,'2026-01-01','0.1.0','bootstrap-v1')"))) return 1;
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("legacy-test"));
        if (!ProjectStore::ensureManagerSchema(legacyPath, &error)
            || !ProjectStore::pipelineModes(legacyPath, &modes, &error)
            || QDir(legacy.path()).entryList({QStringLiteral("project-before-v2-*.sqlite")}, QDir::Files).size() != 1
            || QDir(legacy.path()).entryList({QStringLiteral("project-before-v3-*.sqlite")}, QDir::Files).size() != 1
            || QDir(legacy.path()).entryList({QStringLiteral("project-before-v4-*.sqlite")}, QDir::Files).size() != 1) {
            err << "migration failed: " << error << Qt::endl;
            return 1;
        }
        out << "self-test passed" << Qt::endl;
        return 0;
    }
    err << "Usage: InspectionCli create <existing-empty-directory> | info <project.sqlite> | add-facility <project.sqlite> <name> <type> | add-inspection <project.sqlite> <facility-id> <name> <type> | self-test" << Qt::endl;
    return 2;
}
