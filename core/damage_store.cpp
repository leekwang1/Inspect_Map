#include "damage_store.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMap>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>

#include <algorithm>

namespace {
QString id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QString json(const QJsonValue &value) {
    if (value.isArray()) return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
}
QString normalizedName(const QString &name) {
    QString stem = QFileInfo(name).completeBaseName().toCaseFolded();
    if (stem.endsWith(QStringLiteral("_undistorted"))) stem.chop(12);
    return stem;
}
bool execute(QSqlQuery &query, QString *error) {
    if (query.exec()) return true;
    if (error) *error = query.lastError().text();
    return false;
}
template <typename F> bool withDb(const QString &path, QString *error, F body) {
    if (!QFileInfo(path).isFile()) {
        if (error) *error = QStringLiteral("Project database not found: ") + path;
        return false;
    }
    const QString connection = id();
    bool ok = false;
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(path);
        if (!db.open()) { if (error) *error = db.lastError().text(); }
        else {
            QSqlQuery pragma(db);
            pragma.exec(QStringLiteral("PRAGMA foreign_keys=ON"));
            ok = body(db);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}
struct Match { QString id; QString name; int width = 0; int height = 0; };
bool isTarget(const QString &type) { return type.startsWith(QStringLiteral("target_")); }
QString initialReview(const QString &type, const QJsonObject &llm) {
    if (isTarget(type)) return QStringLiteral("UNREVIEWED");
    return llm.value(QStringLiteral("label")).toString() == QStringLiteral("확실") ?
        QStringLiteral("ACCEPTED") : QStringLiteral("REJECTED");
}
}

bool DamageStore::ensureSchema(const QString &projectDb, QString *error) {
    return withDb(projectDb, error, [&](QSqlDatabase &db) {
        const QStringList sql{
            QStringLiteral("CREATE TABLE IF NOT EXISTS damage_result ("
                "result_id TEXT PRIMARY KEY, dataset_id TEXT NOT NULL REFERENCES dataset(dataset_id), "
                "image_id TEXT NOT NULL REFERENCES image(image_id), source_file TEXT NOT NULL, "
                "schema_name TEXT NOT NULL, imported_at TEXT NOT NULL, image_gsd REAL, "
                "result_json TEXT NOT NULL, active INTEGER NOT NULL DEFAULT 1, "
                "initial_review_policy INTEGER NOT NULL DEFAULT 0)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_damage_result_image ON damage_result(image_id,active)"),
            QStringLiteral("CREATE UNIQUE INDEX IF NOT EXISTS idx_damage_one_active "
                "ON damage_result(image_id) WHERE active=1"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS damage_object ("
                "object_id TEXT PRIMARY KEY, result_id TEXT NOT NULL REFERENCES damage_result(result_id), "
                "source_id TEXT NOT NULL, type TEXT NOT NULL, type_label TEXT NOT NULL, "
                "bbox_json TEXT NOT NULL, geometry_json TEXT NOT NULL, measurements_json TEXT NOT NULL, "
                "llm_review_json TEXT NOT NULL, flags_json TEXT NOT NULL, "
                "review_state TEXT NOT NULL DEFAULT 'UNREVIEWED', override_type TEXT NOT NULL DEFAULT '', "
                "note TEXT NOT NULL DEFAULT '', review_confirmed INTEGER NOT NULL DEFAULT 0)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_damage_object_result ON damage_object(result_id)")
        };
        for (const auto &statement : sql) {
            QSqlQuery query(db);
            if (!query.exec(statement)) {
                if (error) *error = query.lastError().text();
                return false;
            }
        }
        QSqlQuery oldResults(db);
        if (!oldResults.exec(QStringLiteral("SELECT result_id FROM damage_result "
                "WHERE active=1 AND initial_review_policy=0"))) {
            if (error) *error = oldResults.lastError().text();
            return false;
        }
        QStringList resultIds;
        while (oldResults.next()) resultIds.append(oldResults.value(0).toString());
        oldResults.finish();
        if (resultIds.isEmpty()) return true;
        if (!db.transaction()) { if (error) *error = db.lastError().text(); return false; }
        for (const auto &resultId : resultIds) {
            QSqlQuery objects(db);
            objects.prepare(QStringLiteral("SELECT object_id,type,llm_review_json,source_id FROM damage_object "
                "WHERE result_id=? AND review_state='UNREVIEWED' AND review_confirmed=0"));
            objects.addBindValue(resultId);
            if (!execute(objects, error)) { db.rollback(); return false; }
            QList<QPair<QString, QString>> defaults;
            while (objects.next()) {
                const QString type = objects.value(1).toString();
                if (!isTarget(type) && !objects.value(3).toString().startsWith(QStringLiteral("manual")))
                    defaults.append({objects.value(0).toString(),
                    initialReview(type, QJsonDocument::fromJson(objects.value(2).toByteArray()).object())});
            }
            objects.finish();
            QSqlQuery update(db);
            update.prepare(QStringLiteral("UPDATE damage_object SET review_state=? WHERE object_id=?"));
            for (const auto &entry : defaults) {
                update.bindValue(0, entry.second);
                update.bindValue(1, entry.first);
                if (!execute(update, error)) { db.rollback(); return false; }
            }
            QSqlQuery mark(db);
            mark.prepare(QStringLiteral("UPDATE damage_result SET initial_review_policy=1 WHERE result_id=?"));
            mark.addBindValue(resultId);
            if (!execute(mark, error)) { db.rollback(); return false; }
        }
        if (!db.commit()) { if (error) *error = db.lastError().text(); db.rollback(); return false; }
        return true;
    });
}

bool DamageStore::datasets(const QString &projectDb, const QString &inspectionId,
                           QList<DamageDataset> *items, QString *error) {
    if (!items) return false;
    items->clear();
    return withDb(projectDb, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("SELECT d.dataset_id,d.name,count(i.image_id) FROM dataset d "
            "LEFT JOIN image i ON i.dataset_id=d.dataset_id "
            "WHERE (?='' OR d.inspection_id=? OR d.inspection_id IS NULL) "
            "GROUP BY d.dataset_id,d.name ORDER BY d.created_at DESC"));
        const QString scope = inspectionId.isEmpty() ? QStringLiteral("") : inspectionId;
        query.addBindValue(scope);
        query.addBindValue(scope);
        if (!execute(query, error)) return false;
        while (query.next()) items->append({query.value(0).toString(), query.value(1).toString(),
            query.value(2).toInt()});
        return true;
    });
}

bool DamageStore::images(const QString &projectDb, const QString &datasetId,
                         QList<DamageImage> *items, QString *error) {
    if (!items) return false;
    items->clear();
    return withDb(projectDb, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("SELECT i.image_id,COALESCE(NULLIF(i.display_name,''),i.source_identifier),"
            "COALESCE((SELECT v.storage_path FROM image_variant v WHERE v.image_id=i.image_id "
            "AND v.variant_type='DEVICE_UNDISTORTED' AND v.status='VALID' AND v.is_active=1 "
            "ORDER BY v.rowid DESC LIMIT 1),i.raw_path),i.width,i.height,"
            "COALESCE(r.result_id,''),COALESCE(r.source_file,''),COALESCE(r.image_gsd,0),"
            "(SELECT count(*) FROM damage_object o WHERE o.result_id=r.result_id),"
            "(SELECT count(*) FROM damage_object o WHERE o.result_id=r.result_id "
            "AND o.review_confirmed=0 AND substr(o.type,1,7)!='target_') "
            "FROM image i LEFT JOIN damage_result r ON r.image_id=i.image_id AND r.active=1 "
            "WHERE i.dataset_id=? ORDER BY i.capture_time,i.display_name,i.image_id"));
        query.addBindValue(datasetId);
        if (!execute(query, error)) return false;
        const QDir project = QFileInfo(projectDb).absoluteDir();
        while (query.next()) items->append({query.value(0).toString(), query.value(1).toString(),
            project.filePath(query.value(2).toString()), query.value(5).toString(),
            query.value(6).toString(), query.value(3).toInt(), query.value(4).toInt(),
            query.value(8).toInt(), query.value(7).toDouble(), query.value(9).toInt()});
        return true;
    });
}

bool DamageStore::objects(const QString &projectDb, const QString &resultId,
                          QList<DamageObject> *items, QString *error) {
    if (!items) return false;
    items->clear();
    return withDb(projectDb, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("SELECT object_id,source_id,type,type_label,bbox_json,geometry_json,"
            "measurements_json,llm_review_json,flags_json,review_state,override_type,note,review_confirmed "
            "FROM damage_object WHERE result_id=? ORDER BY rowid"));
        query.addBindValue(resultId);
        if (!execute(query, error)) return false;
        while (query.next()) items->append({query.value(0).toString(), query.value(1).toString(),
            query.value(2).toString(), query.value(3).toString(),
            QJsonDocument::fromJson(query.value(4).toByteArray()).array(),
            QJsonDocument::fromJson(query.value(5).toByteArray()).object(),
            QJsonDocument::fromJson(query.value(6).toByteArray()).object(),
            QJsonDocument::fromJson(query.value(7).toByteArray()).object(),
            QJsonDocument::fromJson(query.value(8).toByteArray()).array(),
            query.value(9).toString(), query.value(10).toString(), query.value(11).toString(),
            query.value(12).toBool()});
        return true;
    });
}

bool DamageStore::importFolder(const QString &projectDb, const QString &datasetId,
                               const QString &folder, DamageImportReport *report, QString *error) {
    if (!report) return false;
    *report = {};
    if (!QDir(folder).exists()) { if (error) *error = QStringLiteral("JSON folder not found."); return false; }
    if (!ensureSchema(projectDb, error)) return false;
    return withDb(projectDb, error, [&](QSqlDatabase &db) {
        QMap<QString, QList<Match>> matches;
        QSqlQuery images(db);
        images.prepare(QStringLiteral("SELECT image_id,COALESCE(NULLIF(display_name,''),source_identifier),"
            "width,height FROM image WHERE dataset_id=?"));
        images.addBindValue(datasetId);
        if (!execute(images, error)) return false;
        while (images.next()) {
            Match match{images.value(0).toString(), images.value(1).toString(),
                images.value(2).toInt(), images.value(3).toInt()};
            matches[normalizedName(match.name)].append(match);
        }
        images.finish();
        if (matches.isEmpty()) { if (error) *error = QStringLiteral("Selected data set has no images."); return false; }
        QStringList files;
        QDirIterator it(folder, {QStringLiteral("*.json")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) files.append(it.next());
        files.sort(Qt::CaseInsensitive);
        if (files.isEmpty()) { if (error) *error = QStringLiteral("No JSON files found."); return false; }
        if (!db.transaction()) { if (error) *error = db.lastError().text(); return false; }
        QSet<QString> seenImages;
        for (const auto &filePath : files) {
            QFile file(filePath);
            if (!file.open(QIODevice::ReadOnly)) { report->warnings << QFileInfo(filePath).fileName() + QStringLiteral(": unreadable"); continue; }
            QJsonParseError parseError;
            const auto root = QJsonDocument::fromJson(file.readAll(), &parseError).object();
            const auto image = root.value(QStringLiteral("image")).toObject();
            const auto entries = root.value(QStringLiteral("objects")).toObject();
            const QString schema = root.value(QStringLiteral("schema")).toString();
            const QString name = image.value(QStringLiteral("file")).toString();
            const QString fileName = QFileInfo(filePath).fileName();
            if (parseError.error != QJsonParseError::NoError || schema != QStringLiteral("aigraphy.damage.result.v1") ||
                !root.value(QStringLiteral("coordinate_system")).toString().startsWith(QStringLiteral("image_pixels")) ||
                name.isEmpty() || entries.isEmpty()) {
                report->warnings << fileName + QStringLiteral(": unsupported or empty result"); continue;
            }
            const auto candidates = matches.value(normalizedName(name));
            if (candidates.size() != 1) {
                report->warnings << fileName + (candidates.isEmpty() ? QStringLiteral(": image not matched") :
                    QStringLiteral(": ambiguous image name"));
                continue;
            }
            const auto match = candidates.first();
            if (seenImages.contains(match.id)) {
                report->warnings << fileName + QStringLiteral(": duplicate result for image"); continue;
            }
            if (match.width != image.value(QStringLiteral("width")).toInt() ||
                match.height != image.value(QStringLiteral("height")).toInt()) {
                report->warnings << fileName + QStringLiteral(": image dimensions differ"); continue;
            }
            const QString resultId = id();
            QSqlQuery deactivate(db);
            deactivate.prepare(QStringLiteral("UPDATE damage_result SET active=0 WHERE image_id=? AND active=1"));
            deactivate.addBindValue(match.id);
            bool ok = execute(deactivate, error);
            QSqlQuery insert(db);
            if (ok) {
                insert.prepare(QStringLiteral("INSERT INTO damage_result "
                    "(result_id,dataset_id,image_id,source_file,schema_name,imported_at,image_gsd,"
                    "result_json,active,initial_review_policy) VALUES(?,?,?,?,?,?,?,?,1,1)"));
                insert.addBindValue(resultId);
                insert.addBindValue(datasetId);
                insert.addBindValue(match.id);
                insert.addBindValue(filePath);
                insert.addBindValue(schema);
                insert.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
                insert.addBindValue(image.value(QStringLiteral("gsd_mm_per_px")).toDouble());
                insert.addBindValue(json(root));
                ok = execute(insert, error);
            }
            int objectCount = 0;
            for (auto key = entries.begin(); ok && key != entries.end(); ++key) {
                for (const auto &value : key.value().toArray()) {
                    const auto object = value.toObject();
                    const auto bbox = object.value(QStringLiteral("bbox")).toArray();
                    if (bbox.size() != 4 || !bbox[0].isDouble() || !bbox[1].isDouble() ||
                        !bbox[2].isDouble() || !bbox[3].isDouble()) {
                        report->warnings << fileName + QStringLiteral(": invalid object bbox");
                        continue;
                    }
                    QSqlQuery item(db);
                    item.prepare(QStringLiteral("INSERT INTO damage_object "
                        "(object_id,result_id,source_id,type,type_label,bbox_json,geometry_json,"
                        "measurements_json,llm_review_json,flags_json,review_state) "
                        "VALUES(?,?,?,?,?,?,?,?,?,?,?)"));
                    item.addBindValue(id());
                    item.addBindValue(resultId);
                    item.addBindValue(object.value(QStringLiteral("id")).toString());
                    item.addBindValue(key.key());
                    item.addBindValue(object.value(QStringLiteral("type_label")).toString(key.key()));
                    item.addBindValue(json(bbox));
                    item.addBindValue(json(object.value(QStringLiteral("geometry"))));
                    item.addBindValue(json(object.value(QStringLiteral("measurements"))));
                    item.addBindValue(json(object.value(QStringLiteral("llm_review"))));
                    item.addBindValue(json(object.value(QStringLiteral("flags"))));
                    item.addBindValue(initialReview(key.key(), object.value(QStringLiteral("llm_review")).toObject()));
                    ok = execute(item, error);
                    if (!ok) break;
                    ++objectCount;
                }
            }
            if (!ok) {
                db.rollback();
                return false;
            }
            seenImages.insert(match.id);
            ++report->images;
            report->objects += objectCount;
        }
        if (report->images > 0) {
            QSqlQuery scope(db);
            scope.prepare(QStringLiteral("SELECT inspection_id FROM dataset WHERE dataset_id=?"));
            scope.addBindValue(datasetId);
            if (!execute(scope, error) || !scope.next()) { db.rollback(); return false; }
            const QVariant inspectionId = scope.value(0);
            const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
            QSqlQuery run(db);
            run.prepare(QStringLiteral("INSERT INTO module_run VALUES(?,?,?,?,?,?,?)"));
            run.addBindValue(id());
            run.addBindValue(inspectionId);
            run.addBindValue(QStringLiteral("ai_damage"));
            run.addBindValue(QStringLiteral("REVIEW"));
            run.addBindValue(now);
            run.addBindValue(now);
            run.addBindValue(QStringLiteral("Imported %1 images, %2 objects").arg(report->images).arg(report->objects));
            if (!execute(run, error)) { db.rollback(); return false; }
        }
        if (db.commit()) return true;
        if (error) *error = db.lastError().text();
        db.rollback();
        return false;
    });
}

bool DamageStore::setReview(const QString &projectDb, const QString &objectId,
                            const QString &state, const QString &type, const QString &note, QString *error) {
    if (state != QStringLiteral("UNREVIEWED") && state != QStringLiteral("ACCEPTED") &&
        state != QStringLiteral("REJECTED") && state != QStringLiteral("HOLD")) {
        if (error) *error = QStringLiteral("Invalid review state.");
        return false;
    }
    return withDb(projectDb, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("UPDATE damage_object SET review_state=?,override_type=?,note=?,review_confirmed=1 "
            "WHERE object_id=? AND result_id IN (SELECT result_id FROM damage_result WHERE active=1)"));
        query.addBindValue(state);
        query.addBindValue(type.isNull() ? QStringLiteral("") : type);
        query.addBindValue(note.isNull() ? QStringLiteral("") : note);
        query.addBindValue(objectId);
        if (!execute(query, error)) return false;
        if (query.numRowsAffected() == 1) return true;
        if (error) *error = QStringLiteral("Active damage object not found.");
        return false;
    });
}

bool DamageStore::setReviews(const QString &projectDb, const QString &resultId,
                             const QList<DamageReview> &reviews, QString *error) {
    if (resultId.isEmpty()) return false;
    for (const auto &review : reviews) {
        if (review.state != QStringLiteral("UNREVIEWED") && review.state != QStringLiteral("ACCEPTED") &&
            review.state != QStringLiteral("REJECTED") && review.state != QStringLiteral("HOLD")) {
            if (error) *error = QStringLiteral("Invalid review state.");
            return false;
        }
    }
    return withDb(projectDb, error, [&](QSqlDatabase &db) {
        if (!db.transaction()) { if (error) *error = db.lastError().text(); return false; }
        QSqlQuery query(db);
        query.prepare(QStringLiteral("UPDATE damage_object SET review_state=?,override_type=?,note=?,review_confirmed=1 "
            "WHERE object_id=? AND result_id=? AND result_id IN "
            "(SELECT result_id FROM damage_result WHERE active=1)"));
        for (const auto &review : reviews) {
            query.bindValue(0, review.state);
            query.bindValue(1, review.type.isNull() ? QStringLiteral("") : review.type);
            query.bindValue(2, review.note.isNull() ? QStringLiteral("") : review.note);
            query.bindValue(3, review.objectId);
            query.bindValue(4, resultId);
            if (!execute(query, error) || query.numRowsAffected() != 1) {
                if (error && error->isEmpty()) *error = QStringLiteral("Active damage object not found.");
                db.rollback();
                return false;
            }
        }
        if (db.commit()) return true;
        if (error) *error = db.lastError().text();
        db.rollback();
        return false;
    });
}

bool DamageStore::addObject(const QString &projectDb, const QString &resultId,
                            const QString &type, const QJsonArray &bbox, QString *error) {
    if (type.isEmpty() || bbox.size() != 4) return false;
    const QJsonArray ring{QJsonArray{bbox[0], bbox[1]}, QJsonArray{bbox[2], bbox[1]},
        QJsonArray{bbox[2], bbox[3]}, QJsonArray{bbox[0], bbox[3]}};
    const QString geometry = json(QJsonObject{{QStringLiteral("polygon"), QJsonArray{ring}}});
    return withDb(projectDb, error, [&](QSqlDatabase &db) {
        QSqlQuery existing(db);
        existing.prepare(QStringLiteral("SELECT source_id FROM damage_object WHERE result_id=? "
            "AND (source_id='manual' OR source_id LIKE 'manual-%')"));
        existing.addBindValue(resultId);
        if (!execute(existing, error)) return false;
        int nextNumber = 1;
        while (existing.next()) {
            const QString sourceId = existing.value(0).toString();
            if (sourceId == QStringLiteral("manual")) { ++nextNumber; continue; }
            bool numeric = false;
            const int number = sourceId.mid(7).toInt(&numeric);
            if (numeric) nextNumber = std::max(nextNumber, number + 1);
        }
        existing.finish();
        QSqlQuery query(db);
        query.prepare(QStringLiteral("INSERT INTO damage_object "
            "(object_id,result_id,source_id,type,type_label,bbox_json,geometry_json,"
            "measurements_json,llm_review_json,flags_json,review_state,review_confirmed) "
            "SELECT ?,result_id,?,?,?,?,?,'{}','{}','[]','ACCEPTED',1 "
            "FROM damage_result WHERE result_id=? AND active=1"));
        query.addBindValue(id());
        query.addBindValue(QStringLiteral("manual-%1").arg(nextNumber, 3, 10, QLatin1Char('0')));
        query.addBindValue(type);
        query.addBindValue(type);
        query.addBindValue(json(bbox));
        query.addBindValue(geometry);
        query.addBindValue(resultId);
        if (!execute(query, error)) return false;
        if (query.numRowsAffected() == 1) return true;
        if (error) *error = QStringLiteral("Active damage result not found.");
        return false;
    });
}
