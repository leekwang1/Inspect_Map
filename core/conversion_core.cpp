#include "conversion_core.h"
#include "input_adapters.h"

#include <QCryptographicHash>
#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <algorithm>

namespace {
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

bool openDatabase(const QString &path, QSqlDatabase *db, const QString &connection, QString *error) {
    if (!QFileInfo::exists(path)) {
        if (error) *error = QStringLiteral("Project database not found.");
        return false;
    }
    *db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    db->setDatabaseName(path);
    if (!db->open()) {
        if (error) *error = db->lastError().text();
        return false;
    }
    QSqlQuery query(*db);
    if (query.exec(QStringLiteral("PRAGMA foreign_keys=ON"))) return true;
    if (error) *error = query.lastError().text();
    return false;
}

bool imageSize(const QString &path, int *width, int *height) {
    QImageReader reader(path);
    reader.setAutoTransform(false);
    const QSize size = reader.size();
    *width = size.width();
    *height = size.height();
    return size.isValid();
}

QString relativePath(const QString &dbPath, const QString &path) {
    return QFileInfo(dbPath).absoluteDir().relativeFilePath(path);
}

QString safeSegment(QString value) {
    value.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral("_"));
    return value.isEmpty() || value == QStringLiteral(".") || value == QStringLiteral("..") ? QStringLiteral("images") : value;
}

QString imageStorageName(const QString &flightId, const QString &fileName) {
    return safeSegment(flightId) + QStringLiteral("/") + safeSegment(fileName);
}

bool execute(QSqlDatabase &db, const QString &statement, QString *error) {
    QSqlQuery query(db);
    if (query.exec(statement)) return true;
    if (error) *error = query.lastError().text();
    return false;
}
}

bool ConversionCore::scan(const QString &root, const QString &adapter, ScanResult *result, QString *error) {
    return InputAdapters::scan(root, adapter, result, error);
}

QString ConversionCore::correctedRelativePath(const QString &datasetId, const QString &profileHash,
                                              const ImportedImage &image) {
    const QString rawPrefix = QStringLiteral("source/images/") + datasetId + QStringLiteral("/");
    const QString relative = image.rawPath.startsWith(rawPrefix) ? image.rawPath.mid(rawPrefix.size()) :
        image.id + QStringLiteral(".jpg");
    const QFileInfo file(relative);
    const QString suffix = file.suffix();
    const QString outputSuffix = suffix.compare(QStringLiteral("jpg"), Qt::CaseInsensitive) == 0 ||
        suffix.compare(QStringLiteral("jpeg"), Qt::CaseInsensitive) == 0 ? suffix : QStringLiteral("jpg");
    return QStringLiteral("derived/images/device_undistorted/") + datasetId + QStringLiteral("/") +
        profileHash + QStringLiteral("/") + file.path() + QStringLiteral("/") + file.completeBaseName() +
        QStringLiteral(".") + outputSuffix;
}

bool ConversionCore::ensureSchema(const QString &projectDb, QString *error) {
    const QString connection = newId();
    bool ok = false;
    {
        QSqlDatabase db;
        if (openDatabase(projectDb, &db, connection, error)) {
            if (db.transaction()) {
                const QStringList statements{
                    QStringLiteral("CREATE TABLE IF NOT EXISTS dataset (dataset_id TEXT PRIMARY KEY, inspection_id TEXT REFERENCES inspection(inspection_id), name TEXT NOT NULL, source_type TEXT NOT NULL, source_root_uri TEXT NOT NULL, status TEXT NOT NULL, created_at TEXT NOT NULL, UNIQUE(inspection_id,source_root_uri))"),
                    QStringLiteral("CREATE TABLE IF NOT EXISTS image (image_id TEXT PRIMARY KEY, dataset_id TEXT NOT NULL REFERENCES dataset(dataset_id), source_identifier TEXT NOT NULL, flight_source_id TEXT, raw_path TEXT NOT NULL, width INTEGER NOT NULL, height INTEGER NOT NULL, include_state TEXT NOT NULL, display_name TEXT, capture_time INTEGER, UNIQUE(dataset_id,source_identifier))"),
                    QStringLiteral("CREATE TABLE IF NOT EXISTS image_variant (image_variant_id TEXT PRIMARY KEY, image_id TEXT NOT NULL REFERENCES image(image_id), variant_type TEXT NOT NULL, storage_path TEXT NOT NULL, calibration_sha256 TEXT, width INTEGER NOT NULL, height INTEGER NOT NULL, orientation_policy TEXT NOT NULL, status TEXT NOT NULL, is_active INTEGER NOT NULL DEFAULT 0, UNIQUE(image_id,variant_type,calibration_sha256))"),
                    QStringLiteral("CREATE TABLE IF NOT EXISTS camera_pose (camera_pose_id TEXT PRIMARY KEY, image_id TEXT NOT NULL REFERENCES image(image_id), pose_stage TEXT NOT NULL, pose_json TEXT NOT NULL, source_kind TEXT NOT NULL, status TEXT NOT NULL, UNIQUE(image_id,pose_stage))"),
                    QStringLiteral("CREATE TABLE IF NOT EXISTS point_cloud (point_cloud_id TEXT PRIMARY KEY, dataset_id TEXT NOT NULL REFERENCES dataset(dataset_id), source_path TEXT NOT NULL, source_scope TEXT NOT NULL, storage_kind TEXT NOT NULL, status TEXT NOT NULL)"),
                    QStringLiteral("CREATE INDEX IF NOT EXISTS idx_image_dataset ON image(dataset_id)"),
                    QStringLiteral("CREATE INDEX IF NOT EXISTS idx_variant_image ON image_variant(image_id,variant_type)")
                };
                ok = true;
                for (const auto &statement : statements) if (!execute(db, statement, error)) { ok = false; break; }
                if (ok) ok = db.commit(); else db.rollback();
            } else if (error) *error = db.lastError().text();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

bool ConversionCore::importDataset(const QString &projectDb, const QString &inspectionId,
                                   const ScanResult &scan, QString *datasetId, QString *error) {
    if (!datasetId || scan.images.isEmpty() || !ensureSchema(projectDb, error)) return false;
    const QString connection = newId();
    bool ok = false;
    {
        QSqlDatabase db;
        if (openDatabase(projectDb, &db, connection, error)) {
            QSqlQuery existing(db);
            existing.prepare(QStringLiteral("SELECT dataset_id FROM dataset WHERE inspection_id IS ? AND source_root_uri=?"));
            existing.addBindValue(inspectionId.isEmpty() ? QVariant() : QVariant(inspectionId));
            existing.addBindValue(scan.root);
            if (existing.exec() && existing.next()) {
                *datasetId = existing.value(0).toString();
                ok = true;
            } else if (db.transaction()) {
                const QString id = newId();
                QSqlQuery dataset(db);
                dataset.prepare(QStringLiteral("INSERT INTO dataset VALUES(?,?,?,?,?,?,?)"));
                for (const auto &value : {id, inspectionId, QFileInfo(scan.root).fileName(), scan.adapter,
                                          scan.root, QStringLiteral("IMPORTED"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)})
                    dataset.addBindValue(value);
                ok = dataset.exec();
                if (!ok && error) *error = dataset.lastError().text();
                const QString imageDir = QFileInfo(projectDb).absoluteDir().filePath(QStringLiteral("source/images/") + id);
                if (ok && !QDir().mkpath(imageDir)) { ok = false; if (error) *error = QStringLiteral("Cannot create image directory."); }
                QSet<QString> usedNames;
                for (const auto &source : scan.images) {
                    if (!ok) break;
                    const QString imageId = newId();
                    const QFileInfo sourceFile(source.sourcePath);
                    QString storageName = imageStorageName(source.flightId, sourceFile.fileName());
                    if (usedNames.contains(storageName.toCaseFolded())) {
                        const QString hash = QString::fromLatin1(QCryptographicHash::hash(source.sourceId.toUtf8(), QCryptographicHash::Sha256).toHex().left(8));
                        storageName = imageStorageName(source.flightId, sourceFile.completeBaseName() +
                            QStringLiteral("_") + hash + QStringLiteral(".") + sourceFile.suffix());
                    }
                    usedNames.insert(storageName.toCaseFolded());
                    const QString target = QDir(imageDir).filePath(storageName);
                    if (!QDir().mkpath(QFileInfo(target).absolutePath())) { ok = false; if (error) *error = QStringLiteral("Cannot create image folder."); break; }
                    if (!QFile::copy(source.sourcePath, target)) { ok = false; if (error) *error = QStringLiteral("Failed to copy: ") + source.sourcePath; break; }
                    QSqlQuery image(db);
                    image.prepare(QStringLiteral("INSERT INTO image(image_id,dataset_id,source_identifier,flight_source_id,raw_path,width,height,include_state,display_name,capture_time) VALUES(?,?,?,?,?,?,?,?,?,?)"));
                    for (const auto &value : {imageId, id, source.sourceId, source.flightId, relativePath(projectDb, target)}) image.addBindValue(value);
                    image.addBindValue(source.width);
                    image.addBindValue(source.height);
                    image.addBindValue(QStringLiteral("INCLUDED"));
                    image.addBindValue(sourceFile.fileName());
                    image.addBindValue(source.captureTime ? QVariant(source.captureTime) : QVariant());
                    ok = image.exec();
                    if (!ok) { if (error) *error = image.lastError().text(); break; }
                    QSqlQuery variant(db);
                    variant.prepare(QStringLiteral("INSERT INTO image_variant VALUES(?,?,?,?,?,?,?,?,?,?)"));
                    for (const auto &value : {newId(), imageId, QStringLiteral("RAW"), relativePath(projectDb, target)}) variant.addBindValue(value);
                    variant.addBindValue(QVariant());
                    variant.addBindValue(source.width);
                    variant.addBindValue(source.height);
                    variant.addBindValue(QStringLiteral("RAW_SENSOR"));
                    variant.addBindValue(QStringLiteral("VALID"));
                    variant.addBindValue(1);
                    ok = variant.exec();
                    if (!ok) { if (error) *error = variant.lastError().text(); break; }
                    if (!source.poseJson.isEmpty() && source.poseJson != QStringLiteral("{}")) {
                        QSqlQuery pose(db);
                        pose.prepare(QStringLiteral("INSERT INTO camera_pose VALUES(?,?,?,?,?,?)"));
                        for (const auto &value : {newId(), imageId, QStringLiteral("INITIAL"), source.poseJson,
                                                  QStringLiteral("SNAPSHOT_JSON"), QStringLiteral("REVIEW")}) pose.addBindValue(value);
                        ok = pose.exec();
                        if (!ok && error) *error = pose.lastError().text();
                    }
                }
                for (const auto &cloud : scan.clouds) {
                    if (!ok) break;
                    QSqlQuery query(db);
                    query.prepare(QStringLiteral("INSERT INTO point_cloud VALUES(?,?,?,?,?,?)"));
                    for (const auto &value : {newId(), id, cloud.sourcePath, cloud.scope,
                                              QStringLiteral("EXTERNAL_REFERENCE"), QStringLiteral("REVIEW")}) query.addBindValue(value);
                    ok = query.exec();
                    if (!ok && error) *error = query.lastError().text();
                }
                if (ok && db.commit()) *datasetId = id;
                else { db.rollback(); ok = false; if (error && error->isEmpty()) *error = db.lastError().text(); }
            } else if (error) *error = db.lastError().text();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

bool ConversionCore::images(const QString &projectDb, const QString &datasetId,
                            QList<ImportedImage> *items, QString *error) {
    if (!items) return false;
    items->clear();
    const QString connection = newId();
    bool ok = false;
    {
        QSqlDatabase db;
        if (openDatabase(projectDb, &db, connection, error)) {
            QSqlQuery query(db);
            query.prepare(QStringLiteral("SELECT i.image_id,i.source_identifier,i.raw_path,i.width,i.height,"
                "EXISTS(SELECT 1 FROM camera_pose p WHERE p.image_id=i.image_id),"
                "COALESCE((SELECT v.storage_path FROM image_variant v WHERE v.image_id=i.image_id AND v.variant_type='DEVICE_UNDISTORTED' AND v.status='VALID' ORDER BY v.rowid DESC LIMIT 1),'') "
                ",COALESCE((SELECT p.pose_json FROM camera_pose p WHERE p.image_id=i.image_id AND p.pose_stage='INITIAL' LIMIT 1),'') "
                ",COALESCE(i.display_name,''),COALESCE(i.capture_time,0) "
                "FROM image i WHERE i.dataset_id=? ORDER BY i.flight_source_id,i.capture_time,i.display_name"));
            query.addBindValue(datasetId);
            ok = query.exec();
            if (!ok && error) *error = query.lastError().text();
            while (ok && query.next()) items->append({query.value(0).toString(), query.value(1).toString(),
                query.value(2).toString(), query.value(6).toString(), query.value(3).toInt(), query.value(4).toInt(),
                query.value(5).toBool(), query.value(7).toString(), query.value(8).toString(), query.value(9).toLongLong()});
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    QCollator collator;
    collator.setNumericMode(true);
    std::stable_sort(items->begin(), items->end(), [&](const ImportedImage &a, const ImportedImage &b) {
        if (a.captureTime != b.captureTime) {
            if (!a.captureTime) return false;
            if (!b.captureTime) return true;
            return a.captureTime < b.captureTime;
        }
        return collator.compare(a.displayName, b.displayName) < 0;
    });
    return ok;
}

bool ConversionCore::datasets(const QString &projectDb, QList<ImportedDataset> *items, QString *error) {
    if (!items || !ensureSchema(projectDb, error)) return false;
    items->clear();
    const QString connection = newId();
    bool ok = false;
    {
        QSqlDatabase db;
        if (openDatabase(projectDb, &db, connection, error)) {
            QSqlQuery query(db);
            ok = query.exec(QStringLiteral("SELECT d.dataset_id,d.name,d.source_type,d.source_root_uri,"
                "(SELECT count(*) FROM image i WHERE i.dataset_id=d.dataset_id),"
                "(SELECT count(*) FROM camera_pose p JOIN image i ON i.image_id=p.image_id WHERE i.dataset_id=d.dataset_id AND p.pose_stage='INITIAL'),"
                "(SELECT count(*) FROM point_cloud c WHERE c.dataset_id=d.dataset_id),"
                "(SELECT count(*) FROM image_variant v JOIN image i ON i.image_id=v.image_id "
                "WHERE i.dataset_id=d.dataset_id AND v.variant_type='DEVICE_UNDISTORTED' AND v.status='VALID' AND v.is_active=1) "
                "FROM dataset d ORDER BY d.created_at DESC"));
            if (!ok && error) *error = query.lastError().text();
            while (ok && query.next()) items->append({query.value(0).toString(), query.value(1).toString(),
                query.value(2).toString(), query.value(3).toString(), query.value(4).toInt(),
                query.value(5).toInt(), query.value(6).toInt(), query.value(7).toInt()});
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

bool ConversionCore::clouds(const QString &projectDb, const QString &datasetId,
                            QList<SourceCloud> *items, QString *error) {
    if (!items) return false;
    items->clear();
    const QString connection = newId();
    bool ok = false;
    {
        QSqlDatabase db;
        if (openDatabase(projectDb, &db, connection, error)) {
            QSqlQuery query(db);
            query.prepare(QStringLiteral("SELECT source_path,source_scope FROM point_cloud WHERE dataset_id=? ORDER BY source_scope"));
            query.addBindValue(datasetId);
            ok = query.exec();
            if (!ok && error) *error = query.lastError().text();
            while (ok && query.next()) items->append({query.value(0).toString(), query.value(1).toString()});
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

bool ConversionCore::activeProfile(const QString &projectDb, const QString &datasetId,
                                   QString *path, QString *error) {
    if (!path) return false;
    path->clear();
    const QString connection = newId();
    bool ok = false;
    {
        QSqlDatabase db;
        if (openDatabase(projectDb, &db, connection, error)) {
            QSqlQuery query(db);
            query.prepare(QStringLiteral("SELECT v.calibration_sha256 FROM image_variant v "
                "JOIN image i ON i.image_id=v.image_id WHERE i.dataset_id=? "
                "AND v.variant_type='DEVICE_UNDISTORTED' AND v.status='VALID' AND v.is_active=1 "
                "ORDER BY v.rowid DESC LIMIT 1"));
            query.addBindValue(datasetId);
            ok = query.exec();
            if (!ok && error) *error = query.lastError().text();
            if (ok && query.next()) {
                const QString hash = query.value(0).toString();
                const QString candidate = QFileInfo(projectDb).absoluteDir().filePath(
                    QStringLiteral("source/calibration/") + hash + QStringLiteral(".json"));
                if (QFileInfo(candidate).isFile()) *path = candidate;
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

bool ConversionCore::registerCorrections(const QString &projectDb, const QString &datasetId,
                                         const QString &profilePath, int *registered, QString *error) {
    QList<ImportedImage> imageList;
    if (!images(projectDb, datasetId, &imageList, error)) return false;
    QFile profile(profilePath);
    if (!profile.open(QIODevice::ReadOnly)) { if (error) *error = profile.errorString(); return false; }
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(profile.readAll(), QCryptographicHash::Sha256).toHex());
    const QString connection = newId();
    bool ok = false;
    int count = 0;
    {
        QSqlDatabase db;
        if (openDatabase(projectDb, &db, connection, error)) {
            if (db.transaction()) {
                ok = true;
                for (const auto &image : imageList) {
                    const QString path = QFileInfo(projectDb).absoluteDir().filePath(
                        correctedRelativePath(datasetId, hash, image));
                    int width = 0, height = 0;
                    if (!QFileInfo(path).isFile() || !imageSize(path, &width, &height) ||
                        width != image.width || height != image.height) continue;
                    QSqlQuery deactivate(db);
                    deactivate.prepare(QStringLiteral("UPDATE image_variant SET is_active=0 WHERE image_id=? AND variant_type='DEVICE_UNDISTORTED'"));
                    deactivate.addBindValue(image.id);
                    if (!deactivate.exec()) { ok = false; if (error) *error = deactivate.lastError().text(); break; }
                    QSqlQuery query(db);
                    query.prepare(QStringLiteral("INSERT INTO image_variant VALUES(?,?,?,?,?,?,?,?,?,?) "
                        "ON CONFLICT(image_id,variant_type,calibration_sha256) DO UPDATE SET "
                        "storage_path=excluded.storage_path,width=excluded.width,height=excluded.height,status='VALID',is_active=1"));
                    for (const auto &value : {newId(), image.id, QStringLiteral("DEVICE_UNDISTORTED"), relativePath(projectDb, path), hash}) query.addBindValue(value);
                    query.addBindValue(image.width);
                    query.addBindValue(image.height);
                    query.addBindValue(QStringLiteral("RAW_SENSOR_EXIF_PRESERVED"));
                    query.addBindValue(QStringLiteral("VALID"));
                    query.addBindValue(1);
                    if (!query.exec()) { ok = false; if (error) *error = query.lastError().text(); break; }
                    ++count;
                }
                if (ok && db.commit()) { if (registered) *registered = count; }
                else { db.rollback(); ok = false; if (error && error->isEmpty()) *error = db.lastError().text(); }
            } else if (error) *error = db.lastError().text();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

bool ConversionCore::restoreReadableNames(const QString &projectDb, const QString &datasetId,
                                         int *restored, QString *error) {
    if (restored) *restored = 0;
    if (!ensureSchema(projectDb, error)) return false;
    const QString connection = newId();
    bool ok = false;
    int count = 0;
    {
        QSqlDatabase db;
        if (openDatabase(projectDb, &db, connection, error)) {
            QSqlQuery dataset(db);
            dataset.prepare(QStringLiteral("SELECT source_root_uri,source_type FROM dataset WHERE dataset_id=?"));
            dataset.addBindValue(datasetId);
            if (!dataset.exec() || !dataset.next()) {
                if (error) *error = dataset.lastError().text().isEmpty() ? QStringLiteral("Dataset not found.") : dataset.lastError().text();
            } else if (db.transaction()) {
                const QString sourceRoot = dataset.value(0).toString();
                const QString adapter = dataset.value(1).toString();
                const QDir project = QFileInfo(projectDb).absoluteDir();
                QSqlQuery images(db);
                images.prepare(QStringLiteral("SELECT image_id,source_identifier,flight_source_id,raw_path "
                    "FROM image WHERE dataset_id=? AND (display_name IS NULL OR display_name='')"));
                images.addBindValue(datasetId);
                ok = images.exec();
                if (!ok && error) *error = images.lastError().text();
                while (ok && images.next()) {
                    const QString imageId = images.value(0).toString();
                    const QString sourceId = images.value(1).toString();
                    const QString flightId = images.value(2).toString();
                    const QString oldRaw = images.value(3).toString();
                    QString name = QFileInfo(sourceId).fileName();
                    qint64 captureTime = 0;
                    if (adapter == QStringLiteral("ELIOS_INSPECTOR5")) {
                        QFile metadata(QDir(sourceRoot).filePath(QStringLiteral("snapshots/") + sourceId + QStringLiteral("/snapshot.json")));
                        if (metadata.open(QIODevice::ReadOnly)) {
                            const auto object = QJsonDocument::fromJson(metadata.readAll()).object();
                            name = QFileInfo(object.value(QStringLiteral("camera_image")).toString()).fileName();
                            captureTime = object.value(QStringLiteral("audit")).toObject()
                                .value(QStringLiteral("created_date")).toVariant().toLongLong();
                        } else name.clear();
                    }
                    if (name.isEmpty()) continue;
                    const QFileInfo oldFile(project.filePath(oldRaw));
                    if (!oldFile.isFile()) { ok = false; if (error) *error = QStringLiteral("Missing project image: ") + oldRaw; break; }
                    const QFileInfo namedFile(name);
                    QString newRaw = QStringLiteral("source/images/") + datasetId + QStringLiteral("/") +
                        imageStorageName(flightId, name);
                    for (int suffix = 1; QFileInfo::exists(project.filePath(newRaw)); ++suffix) {
                        const QString uniqueName = namedFile.completeBaseName() + QStringLiteral("_") +
                            QString::number(suffix) + QStringLiteral(".") + namedFile.suffix();
                        newRaw = QStringLiteral("source/images/") + datasetId + QStringLiteral("/") +
                            imageStorageName(flightId, uniqueName);
                    }
                    const QString newRawAbsolute = project.filePath(newRaw);
                    if (!QDir().mkpath(QFileInfo(newRawAbsolute).absolutePath()) ||
                        !QFile::copy(oldFile.absoluteFilePath(), newRawAbsolute)) {
                        ok = false; if (error) *error = QStringLiteral("Cannot copy image: ") + oldRaw; break;
                    }
                    QSqlQuery variants(db);
                    variants.prepare(QStringLiteral("SELECT image_variant_id,variant_type,storage_path,calibration_sha256 "
                        "FROM image_variant WHERE image_id=?"));
                    variants.addBindValue(imageId);
                    if (!variants.exec()) { ok = false; if (error) *error = variants.lastError().text(); break; }
                    while (ok && variants.next()) {
                        const QString variantId = variants.value(0).toString();
                        const QString type = variants.value(1).toString();
                        const QString oldPath = variants.value(2).toString();
                        QString newPath = newRaw;
                        if (type == QStringLiteral("DEVICE_UNDISTORTED")) {
                            ImportedImage renamed;
                            renamed.id = imageId;
                            renamed.rawPath = newRaw;
                            newPath = correctedRelativePath(datasetId, variants.value(3).toString(), renamed);
                            const QString oldAbsolute = project.filePath(oldPath);
                            const QString newAbsolute = project.filePath(newPath);
                            if (!QFileInfo::exists(oldAbsolute) || !QDir().mkpath(QFileInfo(newAbsolute).absolutePath()) ||
                                (QFileInfo::exists(newAbsolute) && QFileInfo(newAbsolute).size() != QFileInfo(oldAbsolute).size()) ||
                                (!QFileInfo::exists(newAbsolute) && !QFile::copy(oldAbsolute, newAbsolute))) {
                                ok = false; if (error) *error = QStringLiteral("Cannot copy corrected image: ") + oldPath; break;
                            }
                        }
                        QSqlQuery updateVariant(db);
                        updateVariant.prepare(QStringLiteral("UPDATE image_variant SET storage_path=? WHERE image_variant_id=?"));
                        updateVariant.addBindValue(newPath);
                        updateVariant.addBindValue(variantId);
                        if (!updateVariant.exec()) { ok = false; if (error) *error = updateVariant.lastError().text(); }
                    }
                    if (!ok) break;
                    QSqlQuery update(db);
                    update.prepare(QStringLiteral("UPDATE image SET raw_path=?,display_name=?,capture_time=? WHERE image_id=?"));
                    update.addBindValue(newRaw);
                    update.addBindValue(name);
                    update.addBindValue(captureTime ? QVariant(captureTime) : QVariant());
                    update.addBindValue(imageId);
                    if (!update.exec()) { ok = false; if (error) *error = update.lastError().text(); break; }
                    ++count;
                }
                if (ok && db.commit()) { if (restored) *restored = count; }
                else { db.rollback(); ok = false; if (error && error->isEmpty()) *error = db.lastError().text(); }
            } else if (error) *error = db.lastError().text();
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}
