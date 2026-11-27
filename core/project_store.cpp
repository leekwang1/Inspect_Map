#include "project_store.h"

#include <QDateTime>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMatrix3x3>
#include <QQuaternion>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>
#include <QVector3D>
#include <array>
#include <cmath>
#include <functional>

namespace {
QString requiredText(const QString &value) {
    return value.isEmpty() ? QStringLiteral("") : value;
}

bool execute(QSqlDatabase &db, const QString &sql, QString *error) {
    QSqlQuery query(db);
    if (query.exec(sql)) return true;
    if (error) *error = query.lastError().text();
    return false;
}

bool withDatabase(const QString &path, bool readOnly, QString *error,
                  const std::function<bool(QSqlDatabase &)> &operation) {
    if (!QFileInfo::exists(path)) {
        if (error) *error = QStringLiteral("Project database not found.");
        return false;
    }
    const QString connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        if (readOnly) db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        db.setDatabaseName(path);
        if (!db.open()) {
            if (error) *error = db.lastError().text();
        } else {
            execute(db, QStringLiteral("PRAGMA foreign_keys = ON"), error);
            ok = operation(db);
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}
}

QString ProjectStore::databasePath(const QString &directory) {
    return QDir(directory).filePath(QStringLiteral("project.sqlite"));
}

bool ProjectStore::create(const QString &directory, const QString &name, QString *error) {
    return create(directory, name, ProjectSetup{}, error);
}

bool ProjectStore::create(const QString &directory, const QString &name, const ProjectSetup &setup, QString *error) {
    if (directory.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("Project directory is required.");
        return false;
    }
    if (name.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("Project name is required.");
        return false;
    }
    if ((!setup.inspectionName.isEmpty() || !setup.modelRoot.isEmpty() || !setup.cloudPath.isEmpty()) &&
        setup.facilityName.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("Facility name is required for quick setup.");
        return false;
    }
    if (!setup.cloudPath.isEmpty() && setup.inspectionName.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("Inspection name is required for a LAS input.");
        return false;
    }
    if ((!setup.modelRoot.isEmpty() && !QFileInfo(setup.modelRoot).isDir()) ||
        (!setup.cloudPath.isEmpty() && (!QFileInfo(setup.cloudPath).isFile() ||
           QFileInfo(setup.cloudPath).suffix().compare(QStringLiteral("las"), Qt::CaseInsensitive) != 0))) {
        if (error) *error = QStringLiteral("Model directory or LAS file is invalid.");
        return false;
    }
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        if (error) *error = QStringLiteral("QSQLITE driver is unavailable.");
        return false;
    }
    const QDir dir(directory);
    if (!dir.exists() || !dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
        if (error) *error = QStringLiteral("Project directory must exist and be empty.");
        return false;
    }
    const QString connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(databasePath(directory));
        if (!db.open()) {
            if (error) *error = db.lastError().text();
        } else if (execute(db, QStringLiteral("PRAGMA foreign_keys = ON"), error) && db.transaction()) {
            const QStringList schema = {
                QStringLiteral("CREATE TABLE schema_version (version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL, app_version TEXT NOT NULL, checksum TEXT NOT NULL)"),
                QStringLiteral("CREATE TABLE project (project_id TEXT PRIMARY KEY, name TEXT NOT NULL, description TEXT NOT NULL DEFAULT '', default_length_unit TEXT NOT NULL DEFAULT 'm', status TEXT NOT NULL DEFAULT 'DRAFT', created_at TEXT NOT NULL, updated_at TEXT NOT NULL)"),
                QStringLiteral("CREATE TABLE facility (facility_id TEXT PRIMARY KEY, project_id TEXT NOT NULL REFERENCES project(project_id), name TEXT NOT NULL, facility_type TEXT NOT NULL DEFAULT '', address TEXT NOT NULL DEFAULT '', management_code TEXT NOT NULL DEFAULT '', description TEXT NOT NULL DEFAULT '')"),
                QStringLiteral("CREATE TABLE inspection (inspection_id TEXT PRIMARY KEY, facility_id TEXT NOT NULL REFERENCES facility(facility_id), name TEXT NOT NULL, inspection_type TEXT NOT NULL DEFAULT '', started_at TEXT, ended_at TEXT, organization TEXT NOT NULL DEFAULT '', inspector TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'DRAFT')"),
                QStringLiteral("CREATE TABLE facility_input (facility_id TEXT PRIMARY KEY REFERENCES facility(facility_id), model_root TEXT NOT NULL)"),
                QStringLiteral("CREATE TABLE inspection_input (inspection_id TEXT PRIMARY KEY REFERENCES inspection(inspection_id), cloud_path TEXT NOT NULL)"),
                QStringLiteral("CREATE TABLE pipeline_setting (module_id TEXT PRIMARY KEY, execution_mode TEXT NOT NULL CHECK(execution_mode IN ('AUTO','REVIEW','MANUAL','SKIP')))"),
                QStringLiteral("CREATE TABLE module_run (run_id TEXT PRIMARY KEY, inspection_id TEXT REFERENCES inspection(inspection_id), module_id TEXT NOT NULL, status TEXT NOT NULL, started_at TEXT NOT NULL, finished_at TEXT, error_message TEXT NOT NULL DEFAULT '')"),
                QStringLiteral("CREATE INDEX module_run_scope ON module_run(inspection_id,module_id,started_at)"),
                QStringLiteral("CREATE TABLE artifact (artifact_id TEXT PRIMARY KEY, created_by_run_id TEXT NOT NULL REFERENCES module_run(run_id), artifact_type TEXT NOT NULL, storage_path TEXT NOT NULL, status TEXT NOT NULL, created_at TEXT NOT NULL, is_active INTEGER NOT NULL DEFAULT 0 CHECK(is_active IN (0,1)))"),
                QStringLiteral("CREATE TABLE artifact_dependency (parent_artifact_id TEXT NOT NULL REFERENCES artifact(artifact_id), child_artifact_id TEXT NOT NULL REFERENCES artifact(artifact_id), PRIMARY KEY(parent_artifact_id,child_artifact_id))")
            };
            ok = true;
            for (const auto &sql : schema) {
                if (!execute(db, sql, error)) { ok = false; break; }
            }
            const auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
            if (ok) {
                QSqlQuery query(db);
                query.prepare(QStringLiteral("INSERT INTO project(project_id,name,created_at,updated_at) VALUES(?,?,?,?)"));
                query.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
                query.addBindValue(name.trimmed());
                query.addBindValue(now);
                query.addBindValue(now);
                ok = query.exec();
                if (!ok && error) *error = query.lastError().text();
            }
            if (ok) {
                QSqlQuery query(db);
                const QString facilityId = QUuid::createUuid().toString(QUuid::WithoutBraces);
                const QString inspectionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
                if (!setup.facilityName.trimmed().isEmpty()) {
                    query.prepare(QStringLiteral("INSERT INTO facility(facility_id,project_id,name) "
                        "SELECT ?,project_id,? FROM project LIMIT 1"));
                    query.addBindValue(facilityId);
                    query.addBindValue(setup.facilityName.trimmed());
                    ok = query.exec();
                }
                if (ok && !setup.inspectionName.trimmed().isEmpty()) {
                    query.prepare(QStringLiteral("INSERT INTO inspection(inspection_id,facility_id,name) VALUES(?,?,?)"));
                    query.addBindValue(inspectionId);
                    query.addBindValue(facilityId);
                    query.addBindValue(setup.inspectionName.trimmed());
                    ok = query.exec();
                }
                if (ok && !setup.modelRoot.isEmpty()) {
                    query.prepare(QStringLiteral("INSERT INTO facility_input VALUES(?,?)"));
                    query.addBindValue(facilityId);
                    query.addBindValue(QFileInfo(setup.modelRoot).canonicalFilePath());
                    ok = query.exec();
                }
                if (ok && !setup.cloudPath.isEmpty()) {
                    query.prepare(QStringLiteral("INSERT INTO inspection_input VALUES(?,?)"));
                    query.addBindValue(inspectionId);
                    query.addBindValue(QFileInfo(setup.cloudPath).canonicalFilePath());
                    ok = query.exec();
                }
                if (!ok) { if (error) *error = query.lastError().text(); }
            }
            if (ok) {
                QSqlQuery query(db);
                query.prepare(QStringLiteral("INSERT INTO schema_version VALUES(4,?,?,?)"));
                query.addBindValue(now);
                query.addBindValue(QStringLiteral("0.1.0"));
                query.addBindValue(QStringLiteral("bootstrap-v4"));
                ok = query.exec();
                if (!ok && error) *error = query.lastError().text();
            }
            if (ok) ok = db.commit(); else db.rollback();
            if (!ok && error && error->isEmpty()) *error = db.lastError().text();
        } else if (error && error->isEmpty()) {
            *error = db.lastError().text();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    if (!ok) QFile::remove(databasePath(directory));
    return ok;
}

bool ProjectStore::read(const QString &databasePath, ProjectInfo *info, QString *error) {
    if (!info || !QFileInfo::exists(databasePath)) {
        if (error) *error = QStringLiteral("Project database not found.");
        return false;
    }
    const QString connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        db.setDatabaseName(databasePath);
        if (db.open()) {
            QSqlQuery query(db);
            ok = query.exec(QStringLiteral("SELECT project_id,name,description,created_at FROM project LIMIT 1")) && query.next();
            if (ok) {
                info->id = query.value(0).toString();
                info->name = query.value(1).toString();
                info->description = query.value(2).toString();
                info->createdAt = query.value(3).toString();
            } else if (error) *error = query.lastError().text();
        } else if (error) *error = db.lastError().text();
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return ok;
}

bool ProjectStore::facilities(const QString &path, QList<FacilityInfo> *items, QString *error) {
    if (!items) return false;
    items->clear();
    return withDatabase(path, true, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        if (!query.exec(QStringLiteral("SELECT facility_id,name,facility_type,address,management_code,description FROM facility ORDER BY name,facility_id"))) {
            if (error) *error = query.lastError().text();
            return false;
        }
        while (query.next()) items->append({query.value(0).toString(), query.value(1).toString(), query.value(2).toString(),
            query.value(3).toString(), query.value(4).toString(), query.value(5).toString()});
        return true;
    });
}

bool ProjectStore::inspections(const QString &path, const QString &facilityId, QList<InspectionInfo> *items, QString *error) {
    if (!items) return false;
    items->clear();
    return withDatabase(path, true, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("SELECT inspection_id,facility_id,name,inspection_type,status,started_at,ended_at,organization,inspector FROM inspection WHERE facility_id=? ORDER BY name,inspection_id"));
        query.addBindValue(facilityId);
        if (!query.exec()) {
            if (error) *error = query.lastError().text();
            return false;
        }
        while (query.next()) items->append({query.value(0).toString(), query.value(1).toString(), query.value(2).toString(),
            query.value(3).toString(), query.value(4).toString(), query.value(5).toString(), query.value(6).toString(),
            query.value(7).toString(), query.value(8).toString()});
        return true;
    });
}

bool ProjectStore::facilityModelPath(const QString &path, const QString &facilityId, QString *modelRoot, QString *error) {
    if (!modelRoot) return false;
    modelRoot->clear();
    return withDatabase(path, true, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("SELECT model_root FROM facility_input WHERE facility_id=?"));
        query.addBindValue(facilityId);
        if (!query.exec()) { if (error) *error = query.lastError().text(); return false; }
        if (query.next()) *modelRoot = query.value(0).toString();
        return true;
    });
}

bool ProjectStore::inspectionCloudPath(const QString &path, const QString &inspectionId, QString *cloudPath, QString *error) {
    if (!cloudPath) return false;
    cloudPath->clear();
    return withDatabase(path, true, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("SELECT cloud_path FROM inspection_input WHERE inspection_id=?"));
        query.addBindValue(inspectionId);
        if (!query.exec()) { if (error) *error = query.lastError().text(); return false; }
        if (query.next()) *cloudPath = query.value(0).toString();
        return true;
    });
}

bool ProjectStore::setFacilityModelPath(const QString &path, const QString &facilityId, const QString &modelRoot, QString *error) {
    if (facilityId.isEmpty() || (!modelRoot.isEmpty() && !QFileInfo(modelRoot).isDir())) {
        if (error) *error = QStringLiteral("Model directory is invalid.");
        return false;
    }
    return withDatabase(path, false, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        if (modelRoot.isEmpty()) {
            query.prepare(QStringLiteral("DELETE FROM facility_input WHERE facility_id=?"));
            query.addBindValue(facilityId);
        } else {
            query.prepare(QStringLiteral("INSERT INTO facility_input VALUES(?,?) ON CONFLICT(facility_id) "
                "DO UPDATE SET model_root=excluded.model_root"));
            query.addBindValue(facilityId);
            query.addBindValue(QFileInfo(modelRoot).canonicalFilePath());
        }
        if (query.exec()) return true;
        if (error) *error = query.lastError().text();
        return false;
    });
}

bool ProjectStore::setInspectionCloudPath(const QString &path, const QString &inspectionId, const QString &cloudPath, QString *error) {
    if (inspectionId.isEmpty() || (!cloudPath.isEmpty() &&
        (!QFileInfo(cloudPath).isFile() || QFileInfo(cloudPath).suffix().compare(QStringLiteral("las"), Qt::CaseInsensitive) != 0))) {
        if (error) *error = QStringLiteral("LAS file is invalid.");
        return false;
    }
    return withDatabase(path, false, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        if (cloudPath.isEmpty()) {
            query.prepare(QStringLiteral("DELETE FROM inspection_input WHERE inspection_id=?"));
            query.addBindValue(inspectionId);
        } else {
            query.prepare(QStringLiteral("INSERT INTO inspection_input VALUES(?,?) ON CONFLICT(inspection_id) "
                "DO UPDATE SET cloud_path=excluded.cloud_path"));
            query.addBindValue(inspectionId);
            query.addBindValue(QFileInfo(cloudPath).canonicalFilePath());
        }
        if (query.exec()) return true;
        if (error) *error = query.lastError().text();
        return false;
    });
}

bool ProjectStore::addFacility(const QString &path, const QString &name, const QString &type, QString *error) {
    return addFacility(path, FacilityInfo{{}, name, type, {}, {}, {}}, error);
}

bool ProjectStore::addFacility(const QString &path, const FacilityInfo &facility, QString *error, QString *createdId) {
    if (facility.name.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("Facility name is required.");
        return false;
    }
    return withDatabase(path, false, error, [&](QSqlDatabase &db) {
        QSqlQuery project(db);
        if (!project.exec(QStringLiteral("SELECT project_id FROM project LIMIT 1")) || !project.next()) {
            if (error) *error = project.lastError().text();
            return false;
        }
        QSqlQuery query(db);
        const QString newId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        query.prepare(QStringLiteral("INSERT INTO facility(facility_id,project_id,name,facility_type,address,management_code,description) VALUES(?,?,?,?,?,?,?)"));
        query.addBindValue(newId);
        query.addBindValue(project.value(0));
        query.addBindValue(facility.name.trimmed());
        query.addBindValue(requiredText(facility.type.trimmed()));
        query.addBindValue(requiredText(facility.address.trimmed()));
        query.addBindValue(requiredText(facility.managementCode.trimmed()));
        query.addBindValue(requiredText(facility.description.trimmed()));
        if (query.exec()) { if (createdId) *createdId = newId; return true; }
        if (error) *error = query.lastError().text();
        return false;
    });
}

bool ProjectStore::addInspection(const QString &path, const QString &facilityId, const QString &name, const QString &type, QString *error) {
    return addInspection(path, InspectionInfo{{}, facilityId, name, type, {}, {}, {}, {}, {}}, error);
}

bool ProjectStore::addInspection(const QString &path, const InspectionInfo &inspection, QString *error, QString *createdId) {
    if (inspection.facilityId.isEmpty() || inspection.name.trimmed().isEmpty() ||
        (!inspection.startedAt.isEmpty() && !QDate::fromString(inspection.startedAt, Qt::ISODate).isValid()) ||
        (!inspection.endedAt.isEmpty() && !QDate::fromString(inspection.endedAt, Qt::ISODate).isValid()) ||
        (!inspection.startedAt.isEmpty() && !inspection.endedAt.isEmpty() && inspection.startedAt > inspection.endedAt)) {
        if (error) *error = QStringLiteral("Facility, inspection name, or date range is invalid.");
        return false;
    }
    return withDatabase(path, false, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        const QString newId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        query.prepare(QStringLiteral("INSERT INTO inspection(inspection_id,facility_id,name,inspection_type,started_at,ended_at,organization,inspector) VALUES(?,?,?,?,?,?,?,?)"));
        query.addBindValue(newId);
        query.addBindValue(inspection.facilityId);
        query.addBindValue(inspection.name.trimmed());
        query.addBindValue(requiredText(inspection.type.trimmed()));
        query.addBindValue(inspection.startedAt.isEmpty() ? QVariant() : QVariant(inspection.startedAt));
        query.addBindValue(inspection.endedAt.isEmpty() ? QVariant() : QVariant(inspection.endedAt));
        query.addBindValue(requiredText(inspection.organization.trimmed()));
        query.addBindValue(requiredText(inspection.inspector.trimmed()));
        if (query.exec()) { if (createdId) *createdId = newId; return true; }
        if (error) *error = query.lastError().text();
        return false;
    });
}

bool ProjectStore::updateFacility(const QString &path, const FacilityInfo &before, const FacilityInfo &after, QString *error) {
    if (after.name.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("Facility name is required.");
        return false;
    }
    return withDatabase(path, false, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("UPDATE facility SET name=?,facility_type=?,address=?,management_code=?,description=? "
            "WHERE facility_id=? AND name=? AND facility_type=? AND address=? AND management_code=? AND description=?"));
        for (const auto &v : {after.name.trimmed(), requiredText(after.type.trimmed()), requiredText(after.address.trimmed()),
                               requiredText(after.managementCode.trimmed()), requiredText(after.description.trimmed()), before.id,
                               before.name, before.type, before.address, before.managementCode, before.description}) query.addBindValue(v);
        if (!query.exec()) { if (error) *error = query.lastError().text(); return false; }
        if (query.numRowsAffected() == 1) return true;
        if (error) *error = QStringLiteral("Facility changed in another process. Reload before saving.");
        return false;
    });
}

bool ProjectStore::updateInspection(const QString &path, const InspectionInfo &before, const InspectionInfo &after, QString *error) {
    if (after.name.trimmed().isEmpty() ||
        (!after.startedAt.isEmpty() && !QDate::fromString(after.startedAt, Qt::ISODate).isValid()) ||
        (!after.endedAt.isEmpty() && !QDate::fromString(after.endedAt, Qt::ISODate).isValid()) ||
        (!after.startedAt.isEmpty() && !after.endedAt.isEmpty() && after.startedAt > after.endedAt)) {
        if (error) *error = QStringLiteral("Inspection name or date range is invalid.");
        return false;
    }
    return withDatabase(path, false, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("UPDATE inspection SET name=?,inspection_type=?,started_at=?,ended_at=?,organization=?,inspector=? "
            "WHERE inspection_id=? AND name=? AND inspection_type=? AND organization=? AND inspector=? "
            "AND started_at IS ? AND ended_at IS ?"));
        query.addBindValue(after.name.trimmed());
        query.addBindValue(requiredText(after.type.trimmed()));
        query.addBindValue(after.startedAt.isEmpty() ? QVariant() : QVariant(after.startedAt));
        query.addBindValue(after.endedAt.isEmpty() ? QVariant() : QVariant(after.endedAt));
        for (const auto &v : {requiredText(after.organization.trimmed()), requiredText(after.inspector.trimmed()), before.id,
                               before.name, before.type, before.organization, before.inspector}) query.addBindValue(v);
        query.addBindValue(before.startedAt.isEmpty() ? QVariant() : QVariant(before.startedAt));
        query.addBindValue(before.endedAt.isEmpty() ? QVariant() : QVariant(before.endedAt));
        if (!query.exec()) { if (error) *error = query.lastError().text(); return false; }
        if (query.numRowsAffected() == 1) return true;
        if (error) *error = QStringLiteral("Inspection changed in another process. Reload before saving.");
        return false;
    });
}

bool ProjectStore::ensureManagerSchema(const QString &path, QString *error) {
    int version = 0;
    const bool checked = withDatabase(path, true, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        if (!query.exec(QStringLiteral("SELECT max(version) FROM schema_version")) || !query.next()) {
            if (error) *error = query.lastError().text();
            return false;
        }
        version = query.value(0).toInt();
        return true;
    });
    if (!checked) return false;
    if (version == 4) return true;
    if (error) *error = version > 4 ? QStringLiteral("Project database is newer than this application.") :
        QStringLiteral("Unsupported project database version. Create a new project.");
    return false;
}

bool ProjectStore::pipelineModes(const QString &path, QMap<QString, QString> *modes, QString *error) {
    if (!modes) return false;
    modes->clear();
    return withDatabase(path, true, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        if (!query.exec(QStringLiteral("SELECT module_id,execution_mode FROM pipeline_setting"))) {
            if (error) *error = query.lastError().text();
            return false;
        }
        while (query.next()) modes->insert(query.value(0).toString(), query.value(1).toString());
        return true;
    });
}

bool ProjectStore::setPipelineMode(const QString &path, const QString &moduleId, const QString &mode, QString *error) {
    if (moduleId.isEmpty() || !QStringList{QStringLiteral("AUTO"), QStringLiteral("REVIEW"),
                                          QStringLiteral("MANUAL"), QStringLiteral("SKIP")}.contains(mode)) {
        if (error) *error = QStringLiteral("Invalid pipeline module or mode.");
        return false;
    }
    return withDatabase(path, false, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("INSERT INTO pipeline_setting(module_id,execution_mode) VALUES(?,?) "
                                     "ON CONFLICT(module_id) DO UPDATE SET execution_mode=excluded.execution_mode"));
        query.addBindValue(moduleId);
        query.addBindValue(mode);
        if (query.exec()) return true;
        if (error) *error = query.lastError().text();
        return false;
    });
}

bool ProjectStore::runs(const QString &path, const QString &inspectionId, QList<ModuleRunInfo> *items, QString *error) {
    if (!items) return false;
    items->clear();
    return withDatabase(path, true, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("SELECT run_id,inspection_id,module_id,status,started_at,finished_at,error_message FROM module_run "
                                     "WHERE (?='' OR inspection_id=?) ORDER BY started_at DESC,run_id DESC"));
        query.addBindValue(inspectionId);
        query.addBindValue(inspectionId);
        if (!query.exec()) { if (error) *error = query.lastError().text(); return false; }
        while (query.next()) items->append({query.value(0).toString(), query.value(1).toString(), query.value(2).toString(),
            query.value(3).toString(), query.value(4).toString(), query.value(5).toString(), query.value(6).toString()});
        return true;
    });
}

bool ProjectStore::artifacts(const QString &path, QList<ArtifactInfo> *items, QString *error) {
    if (!items) return false;
    items->clear();
    return withDatabase(path, true, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        if (!query.exec(QStringLiteral("SELECT artifact_id,created_by_run_id,artifact_type,storage_path,status,created_at,is_active "
                                       "FROM artifact ORDER BY created_at DESC,artifact_id DESC"))) {
            if (error) *error = query.lastError().text();
            return false;
        }
        while (query.next()) items->append({query.value(0).toString(), query.value(1).toString(), query.value(2).toString(),
            query.value(3).toString(), query.value(4).toString(), query.value(5).toString(), query.value(6).toBool()});
        return true;
    });
}

bool ProjectStore::recordRun(const QString &path, const ModuleRunInfo &run, QString *error) {
    if (run.id.isEmpty() || run.moduleId.isEmpty() || run.startedAt.isEmpty() ||
        !QStringList{QStringLiteral("RUNNING"), QStringLiteral("VALID"), QStringLiteral("REVIEW"),
                     QStringLiteral("FAILED"), QStringLiteral("SKIPPED"), QStringLiteral("CANCELLED")}.contains(run.status)) {
        if (error) *error = QStringLiteral("Invalid run record.");
        return false;
    }
    return withDatabase(path, false, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("INSERT INTO module_run VALUES(?,?,?,?,?,?,?)"));
        query.addBindValue(run.id);
        query.addBindValue(run.inspectionId.isEmpty() ? QVariant() : QVariant(run.inspectionId));
        query.addBindValue(run.moduleId);
        query.addBindValue(run.status);
        query.addBindValue(run.startedAt);
        query.addBindValue(run.finishedAt.isEmpty() ? QVariant() : QVariant(run.finishedAt));
        query.addBindValue(run.error.isNull() ? QStringLiteral("") : run.error);
        if (query.exec()) return true;
        if (error) *error = query.lastError().text();
        return false;
    });
}

bool ProjectStore::finishRun(const QString &path, const QString &runId, const QString &status,
                             const QString &message, QString *error) {
    if (runId.isEmpty() || !QStringList{QStringLiteral("VALID"), QStringLiteral("REVIEW"),
        QStringLiteral("FAILED"), QStringLiteral("CANCELLED")}.contains(status)) {
        if (error) *error = QStringLiteral("Invalid run completion.");
        return false;
    }
    return withDatabase(path, false, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("UPDATE module_run SET status=?,finished_at=?,error_message=? "
                                     "WHERE run_id=? AND status='RUNNING'"));
        query.addBindValue(status);
        query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        query.addBindValue(message.isNull() ? QStringLiteral("") : message);
        query.addBindValue(runId);
        if (query.exec() && query.numRowsAffected() == 1) return true;
        if (error) *error = query.lastError().isValid() ? query.lastError().text() :
            QStringLiteral("Run is not in progress.");
        return false;
    });
}

bool ProjectStore::ensureRegistrationSchema(const QString &path, QString *error) {
    return withDatabase(path, false, error, [&](QSqlDatabase &db) {
        if (!db.transaction()) { if (error) *error = db.lastError().text(); return false; }
        bool ok = execute(db, QStringLiteral("CREATE TABLE IF NOT EXISTS registration_result ("
            "registration_id TEXT PRIMARY KEY, inspection_id TEXT, dataset_id TEXT, point_cloud_id TEXT, "
            "source_cloud_path TEXT NOT NULL, model_root TEXT NOT NULL, method TEXT NOT NULL, "
            "matrix_json TEXT NOT NULL, scale REAL NOT NULL, landmark_rmse REAL, surface_rmse REAL, "
            "overlap REAL, icp_iterations INTEGER, pose_frame_confirmed INTEGER NOT NULL, "
            "created_at TEXT NOT NULL, active INTEGER NOT NULL, survey_side TEXT NOT NULL DEFAULT '', "
            "review_status TEXT NOT NULL DEFAULT 'CONFIRMED', confirmed_at TEXT, run_id TEXT)"), error);
        if (ok && db.commit()) return true;
        if (ok && error) *error = db.lastError().text();
        db.rollback();
        return false;
    });
}

bool ProjectStore::pendingRegistrations(const QString &path, const QString &inspectionId,
                                        QList<RegistrationReviewInfo> *items, QString *error) {
    if (!items) return false;
    items->clear();
    return withDatabase(path, true, error, [&](QSqlDatabase &db) {
        if (!db.tables().contains(QStringLiteral("registration_result")))
            return true;
        QSqlQuery query(db);
        query.prepare(QStringLiteral("SELECT registration_id,inspection_id,dataset_id,source_cloud_path,"
            "model_root,method,matrix_json,survey_side,created_at,pose_frame_confirmed,"
            "surface_rmse,overlap FROM registration_result "
            "WHERE review_status='PENDING' AND (?='' OR inspection_id=? OR inspection_id IS NULL) "
            "ORDER BY created_at DESC,registration_id DESC"));
        query.addBindValue(inspectionId);
        query.addBindValue(inspectionId);
        if (!query.exec()) { if (error) *error = query.lastError().text(); return false; }
        while (query.next()) items->append({query.value(0).toString(), query.value(1).toString(),
            query.value(2).toString(), query.value(3).toString(), query.value(4).toString(),
            query.value(5).toString(), query.value(6).toString(), query.value(7).toString(),
            query.value(8).toString(), query.value(9).toBool(), query.value(10).toDouble(),
            query.value(11).toDouble()});
        return true;
    });
}

bool ProjectStore::confirmRegistration(const QString &path, const QString &registrationId,
                                       int *poseCount, QString *error) {
    if (poseCount) *poseCount = 0;
    if (registrationId.isEmpty()) {
        if (error) *error = QStringLiteral("Registration ID is required.");
        return false;
    }
    return withDatabase(path, false, error, [&](QSqlDatabase &db) {
        if (!db.transaction()) { if (error) *error = db.lastError().text(); return false; }
        auto fail = [&](const QString &message) {
            if (error) *error = message;
            db.rollback();
            return false;
        };
        QSqlQuery selected(db);
        selected.prepare(QStringLiteral("SELECT inspection_id,dataset_id,source_cloud_path,matrix_json,"
            "pose_frame_confirmed,run_id FROM registration_result "
            "WHERE registration_id=? AND review_status='PENDING' AND active=0"));
        selected.addBindValue(registrationId);
        if (!selected.exec()) return fail(selected.lastError().text());
        if (!selected.next()) return fail(QStringLiteral("Pending registration result not found."));
        const QString inspectionId = selected.value(0).toString();
        const QString datasetId = selected.value(1).toString();
        const QString cloudPath = selected.value(2).toString();
        const auto matrixRows = QJsonDocument::fromJson(selected.value(3).toString().toUtf8()).array();
        const bool transformPoses = selected.value(4).toBool();
        const QString runId = selected.value(5).toString();
        std::array<std::array<double, 4>, 4> matrix{};
        if (matrixRows.size() != 4) return fail(QStringLiteral("Invalid registration matrix."));
        for (int row = 0; row < 4; ++row) {
            const auto columns = matrixRows[row].toArray();
            if (columns.size() != 4) return fail(QStringLiteral("Invalid registration matrix."));
            for (int column = 0; column < 4; ++column) {
                if (!columns[column].isDouble() || !std::isfinite(columns[column].toDouble()))
                    return fail(QStringLiteral("Invalid registration matrix."));
                matrix[row][column] = columns[column].toDouble();
            }
        }
        const double scale = std::sqrt(matrix[0][0] * matrix[0][0] +
            matrix[1][0] * matrix[1][0] + matrix[2][0] * matrix[2][0]);
        if (!std::isfinite(scale) || scale < 1e-12)
            return fail(QStringLiteral("Invalid registration scale."));
        QMatrix3x3 rotationMatrix;
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column)
                rotationMatrix(row, column) = float(matrix[row][column] / scale);
        const QQuaternion frameRotation = QQuaternion::fromRotationMatrix(rotationMatrix);

        QSqlQuery deactivate(db);
        deactivate.prepare(QStringLiteral("UPDATE registration_result SET active=0 "
            "WHERE active=1 AND (source_cloud_path=? OR (?<>'' AND dataset_id=?))"));
        deactivate.addBindValue(cloudPath);
        deactivate.addBindValue(datasetId);
        deactivate.addBindValue(datasetId);
        if (!deactivate.exec()) return fail(deactivate.lastError().text());
        int converted = 0;
        if (!datasetId.isEmpty()) {
            QSqlQuery clear(db);
            clear.prepare(QStringLiteral("DELETE FROM camera_pose WHERE pose_stage='MODEL_ALIGNED' "
                "AND image_id IN (SELECT image_id FROM image WHERE dataset_id=?)"));
            clear.addBindValue(datasetId);
            if (!clear.exec()) return fail(clear.lastError().text());
        }
        if (transformPoses) {
            if (datasetId.isEmpty()) return fail(QStringLiteral("Camera data set is missing."));
            QSqlQuery initial(db);
            initial.prepare(QStringLiteral("SELECT p.image_id,p.pose_json FROM camera_pose p "
                "JOIN image i ON i.image_id=p.image_id WHERE i.dataset_id=? AND p.pose_stage='INITIAL'"));
            initial.addBindValue(datasetId);
            if (!initial.exec()) return fail(initial.lastError().text());
            while (initial.next()) {
                QJsonObject pose = QJsonDocument::fromJson(initial.value(1).toString().toUtf8()).object();
                const auto position = pose.value(QStringLiteral("position")).toObject();
                const auto orientation = pose.value(QStringLiteral("orientation")).toObject();
                if (position.isEmpty() || orientation.isEmpty())
                    return fail(QStringLiteral("Invalid initial camera pose."));
                const double x = position.value(QStringLiteral("x")).toDouble();
                const double y = position.value(QStringLiteral("y")).toDouble();
                const double z = position.value(QStringLiteral("z")).toDouble();
                const QQuaternion original(float(orientation.value(QStringLiteral("w")).toDouble()),
                    float(orientation.value(QStringLiteral("x")).toDouble()),
                    float(orientation.value(QStringLiteral("y")).toDouble()),
                    float(orientation.value(QStringLiteral("z")).toDouble()));
                if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || original.isNull())
                    return fail(QStringLiteral("Invalid initial camera pose."));
                const QQuaternion turned = (frameRotation * original.normalized()).normalized();
                pose.insert(QStringLiteral("position"), QJsonObject{
                    {QStringLiteral("x"), matrix[0][0] * x + matrix[0][1] * y + matrix[0][2] * z + matrix[0][3]},
                    {QStringLiteral("y"), matrix[1][0] * x + matrix[1][1] * y + matrix[1][2] * z + matrix[1][3]},
                    {QStringLiteral("z"), matrix[2][0] * x + matrix[2][1] * y + matrix[2][2] * z + matrix[2][3]}});
                pose.insert(QStringLiteral("orientation"), QJsonObject{
                    {QStringLiteral("w"), turned.scalar()}, {QStringLiteral("x"), turned.x()},
                    {QStringLiteral("y"), turned.y()}, {QStringLiteral("z"), turned.z()}});
                pose.insert(QStringLiteral("registration_id"), registrationId);
                QSqlQuery upsert(db);
                upsert.prepare(QStringLiteral("INSERT INTO camera_pose VALUES(?,?,?,?,?,?) "
                    "ON CONFLICT(image_id,pose_stage) DO UPDATE SET pose_json=excluded.pose_json, "
                    "source_kind=excluded.source_kind,status=excluded.status"));
                upsert.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
                upsert.addBindValue(initial.value(0));
                upsert.addBindValue(QStringLiteral("MODEL_ALIGNED"));
                upsert.addBindValue(QString::fromUtf8(QJsonDocument(pose).toJson(QJsonDocument::Compact)));
                upsert.addBindValue(QStringLiteral("REGISTRATION"));
                upsert.addBindValue(QStringLiteral("VALID"));
                if (!upsert.exec()) return fail(upsert.lastError().text());
                ++converted;
            }
            if (converted == 0) return fail(QStringLiteral("No initial camera poses to transform."));
        }
        const QString confirmedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        QSqlQuery confirm(db);
        confirm.prepare(QStringLiteral("UPDATE registration_result SET active=1,review_status='CONFIRMED',"
            "confirmed_at=? WHERE registration_id=? AND review_status='PENDING'"));
        confirm.addBindValue(confirmedAt);
        confirm.addBindValue(registrationId);
        if (!confirm.exec() || confirm.numRowsAffected() != 1)
            return fail(confirm.lastError().isValid() ? confirm.lastError().text() :
                QStringLiteral("Registration confirmation failed."));
        if (!runId.isEmpty()) {
            QSqlQuery run(db);
            run.prepare(QStringLiteral("UPDATE module_run SET status='VALID' WHERE run_id=? AND status='REVIEW'"));
            run.addBindValue(runId);
            if (!run.exec() || run.numRowsAffected() != 1)
                return fail(run.lastError().isValid() ? run.lastError().text() :
                    QStringLiteral("Registration run is not awaiting review."));
        }
        if (converted > 0) {
            QSqlQuery cameraRun(db);
            cameraRun.prepare(QStringLiteral("INSERT INTO module_run VALUES(?,?,?,?,?,?,?)"));
            cameraRun.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
            cameraRun.addBindValue(inspectionId.isEmpty() ? QVariant() : QVariant(inspectionId));
            cameraRun.addBindValue(QStringLiteral("camera_pose"));
            cameraRun.addBindValue(QStringLiteral("VALID"));
            cameraRun.addBindValue(confirmedAt);
            cameraRun.addBindValue(confirmedAt);
            cameraRun.addBindValue(QStringLiteral(""));
            if (!cameraRun.exec()) return fail(cameraRun.lastError().text());
        }
        if (!db.commit()) return fail(db.lastError().text());
        if (poseCount) *poseCount = converted;
        return true;
    });
}

bool ProjectStore::recordArtifact(const QString &path, const ArtifactInfo &artifact, QString *error) {
    if (artifact.id.isEmpty() || artifact.runId.isEmpty() || artifact.type.isEmpty() || artifact.path.isEmpty() ||
        artifact.status != QStringLiteral("VALID") || artifact.active) {
        if (error) *error = QStringLiteral("Invalid artifact record; adoption is a separate action.");
        return false;
    }
    const auto absolute = QFileInfo(path).absoluteDir().absoluteFilePath(artifact.path);
    const auto root = QFileInfo(path).absoluteDir().canonicalPath() + QLatin1Char('/');
    const auto canonical = QFileInfo(absolute).canonicalFilePath();
    if (QDir::isAbsolutePath(artifact.path) || !canonical.startsWith(root, Qt::CaseInsensitive)) {
        if (error) *error = QStringLiteral("Artifact path must remain within the project directory.");
        return false;
    }
    if (!QFileInfo::exists(canonical)) {
        if (error) *error = QStringLiteral("Artifact file not found.");
        return false;
    }
    return withDatabase(path, false, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("INSERT INTO artifact VALUES(?,?,?,?,?,?,0)"));
        for (const auto &value : {artifact.id, artifact.runId, artifact.type,
                                  artifact.path, artifact.status, artifact.createdAt}) query.addBindValue(value);
        if (query.exec()) return true;
        if (error) *error = query.lastError().text();
        return false;
    });
}

bool ProjectStore::adoptArtifact(const QString &path, const QString &artifactId, QString *error) {
    return withDatabase(path, false, error, [&](QSqlDatabase &db) {
        if (!db.transaction()) { if (error) *error = db.lastError().text(); return false; }
        QSqlQuery selected(db);
        selected.prepare(QStringLiteral("SELECT a.artifact_type,r.inspection_id,a.status,r.status FROM artifact a "
                                        "JOIN module_run r ON r.run_id=a.created_by_run_id WHERE a.artifact_id=?"));
        selected.addBindValue(artifactId);
        if (!selected.exec() || !selected.next() || selected.value(2).toString() != QStringLiteral("VALID") ||
            selected.value(3).toString() != QStringLiteral("VALID")) {
            if (error) *error = QStringLiteral("Only a VALID artifact from a VALID run can be adopted.");
            db.rollback();
            return false;
        }
        QSqlQuery file(db);
        file.prepare(QStringLiteral("SELECT storage_path FROM artifact WHERE artifact_id=?"));
        file.addBindValue(artifactId);
        if (!file.exec() || !file.next() || !QFileInfo(QFileInfo(path).absoluteDir().filePath(file.value(0).toString())).exists()) {
            if (error) *error = QStringLiteral("Artifact file is missing.");
            db.rollback();
            return false;
        }
        QSqlQuery current(db);
        current.prepare(QStringLiteral("SELECT a.artifact_id FROM artifact a JOIN module_run r ON r.run_id=a.created_by_run_id "
                                       "WHERE a.is_active=1 AND a.artifact_type=? AND r.inspection_id IS ?"));
        current.addBindValue(selected.value(0));
        current.addBindValue(selected.value(1));
        if (!current.exec()) { if (error) *error = current.lastError().text(); db.rollback(); return false; }
        QStringList previous;
        while (current.next()) previous.append(current.value(0).toString());
        QSqlQuery stale(db);
        stale.prepare(QStringLiteral("WITH RECURSIVE descendants(id) AS ("
            "SELECT child_artifact_id FROM artifact_dependency WHERE parent_artifact_id=? "
            "UNION SELECT d.child_artifact_id FROM artifact_dependency d JOIN descendants x ON d.parent_artifact_id=x.id) "
            "UPDATE artifact SET status='STALE',is_active=0 WHERE artifact_id IN (SELECT id FROM descendants)"));
        for (const auto &oldId : previous) {
            if (oldId == artifactId) continue;
            stale.bindValue(0, oldId);
            if (!stale.exec()) { if (error) *error = stale.lastError().text(); db.rollback(); return false; }
        }
        QSqlQuery candidate(db);
        candidate.prepare(QStringLiteral("SELECT status FROM artifact WHERE artifact_id=?"));
        candidate.addBindValue(artifactId);
        if (!candidate.exec() || !candidate.next() || candidate.value(0).toString() != QStringLiteral("VALID")) {
            if (error) *error = QStringLiteral("Candidate depends on a superseded result.");
            db.rollback();
            return false;
        }
        QSqlQuery clear(db);
        clear.prepare(QStringLiteral("UPDATE artifact SET is_active=0 WHERE artifact_id=?"));
        for (const auto &oldId : previous) {
            clear.bindValue(0, oldId);
            if (!clear.exec()) { if (error) *error = clear.lastError().text(); db.rollback(); return false; }
        }
        QSqlQuery adopt(db);
        adopt.prepare(QStringLiteral("UPDATE artifact SET is_active=1 WHERE artifact_id=? AND status='VALID'"));
        adopt.addBindValue(artifactId);
        if (!adopt.exec() || adopt.numRowsAffected() != 1) {
            if (error) *error = adopt.lastError().text();
            db.rollback();
            return false;
        }
        if (db.commit()) return true;
        if (error) *error = db.lastError().text();
        return false;
    });
}

bool ProjectStore::linkArtifacts(const QString &path, const QString &parentId, const QString &childId, QString *error) {
    if (parentId.isEmpty() || childId.isEmpty() || parentId == childId) {
        if (error) *error = QStringLiteral("Invalid artifact dependency.");
        return false;
    }
    return withDatabase(path, false, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("INSERT INTO artifact_dependency VALUES(?,?)"));
        query.addBindValue(parentId);
        query.addBindValue(childId);
        if (query.exec()) return true;
        if (error) *error = query.lastError().text();
        return false;
    });
}
