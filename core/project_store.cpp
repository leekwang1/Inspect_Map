#include "project_store.h"

#include <QDateTime>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
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
    if (directory.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("Project directory is required.");
        return false;
    }
    if (name.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("Project name is required.");
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
                query.prepare(QStringLiteral("INSERT INTO schema_version VALUES(3,?,?,?)"));
                query.addBindValue(now);
                query.addBindValue(QStringLiteral("0.1.0"));
                query.addBindValue(QStringLiteral("bootstrap-v3"));
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

bool ProjectStore::addFacility(const QString &path, const QString &name, const QString &type, QString *error) {
    return addFacility(path, FacilityInfo{{}, name, type, {}, {}, {}}, error);
}

bool ProjectStore::addFacility(const QString &path, const FacilityInfo &facility, QString *error) {
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
        query.prepare(QStringLiteral("INSERT INTO facility(facility_id,project_id,name,facility_type,address,management_code,description) VALUES(?,?,?,?,?,?,?)"));
        query.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
        query.addBindValue(project.value(0));
        query.addBindValue(facility.name.trimmed());
        query.addBindValue(requiredText(facility.type.trimmed()));
        query.addBindValue(requiredText(facility.address.trimmed()));
        query.addBindValue(requiredText(facility.managementCode.trimmed()));
        query.addBindValue(requiredText(facility.description.trimmed()));
        if (query.exec()) return true;
        if (error) *error = query.lastError().text();
        return false;
    });
}

bool ProjectStore::addInspection(const QString &path, const QString &facilityId, const QString &name, const QString &type, QString *error) {
    return addInspection(path, InspectionInfo{{}, facilityId, name, type, {}, {}, {}, {}, {}}, error);
}

bool ProjectStore::addInspection(const QString &path, const InspectionInfo &inspection, QString *error) {
    if (inspection.facilityId.isEmpty() || inspection.name.trimmed().isEmpty() ||
        (!inspection.startedAt.isEmpty() && !QDate::fromString(inspection.startedAt, Qt::ISODate).isValid()) ||
        (!inspection.endedAt.isEmpty() && !QDate::fromString(inspection.endedAt, Qt::ISODate).isValid()) ||
        (!inspection.startedAt.isEmpty() && !inspection.endedAt.isEmpty() && inspection.startedAt > inspection.endedAt)) {
        if (error) *error = QStringLiteral("Facility, inspection name, or date range is invalid.");
        return false;
    }
    return withDatabase(path, false, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("INSERT INTO inspection(inspection_id,facility_id,name,inspection_type,started_at,ended_at,organization,inspector) VALUES(?,?,?,?,?,?,?,?)"));
        query.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
        query.addBindValue(inspection.facilityId);
        query.addBindValue(inspection.name.trimmed());
        query.addBindValue(requiredText(inspection.type.trimmed()));
        query.addBindValue(inspection.startedAt.isEmpty() ? QVariant() : QVariant(inspection.startedAt));
        query.addBindValue(inspection.endedAt.isEmpty() ? QVariant() : QVariant(inspection.endedAt));
        query.addBindValue(requiredText(inspection.organization.trimmed()));
        query.addBindValue(requiredText(inspection.inspector.trimmed()));
        if (query.exec()) return true;
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

bool ProjectStore::backup(const QString &path, const QString &destination, QString *error) {
    if (QFileInfo::exists(destination) || QFileInfo(destination).absoluteFilePath() == QFileInfo(path).absoluteFilePath()) {
        if (error) *error = QStringLiteral("Backup destination already exists or matches the project database.");
        return false;
    }
    return withDatabase(path, true, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        query.prepare(QStringLiteral("VACUUM INTO ?"));
        query.addBindValue(destination);
        if (query.exec()) return true;
        if (error) *error = query.lastError().text();
        return false;
    });
}

bool ProjectStore::integrityCheck(const QString &path, QString *result, QString *error) {
    if (!result) return false;
    return withDatabase(path, true, error, [&](QSqlDatabase &db) {
        QSqlQuery integrity(db);
        if (!integrity.exec(QStringLiteral("PRAGMA integrity_check")) || !integrity.next()) {
            if (error) *error = integrity.lastError().text();
            return false;
        }
        const QString value = integrity.value(0).toString();
        QSqlQuery foreignKeys(db);
        if (!foreignKeys.exec(QStringLiteral("PRAGMA foreign_key_check"))) {
            if (error) *error = foreignKeys.lastError().text();
            return false;
        }
        const bool fkError = foreignKeys.next();
        *result = QStringLiteral("SQLite integrity: %1\nForeign keys: %2")
            .arg(value, fkError ? QStringLiteral("error") : QStringLiteral("ok"));
        return value == QStringLiteral("ok") && !fkError;
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
    if (version > 4) {
        if (error) *error = QStringLiteral("Project database is newer than this application.");
        return false;
    }
    if (version >= 3) return true;
    if (version != 1 && version != 2) {
        if (error) *error = QStringLiteral("Unsupported project database version.");
        return false;
    }
    const auto backupPath = QFileInfo(path).absoluteDir().filePath(
        QStringLiteral("project-before-v%1-%2.sqlite").arg(version + 1).arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!backup(path, backupPath, error)) return false;
    const bool migrated = withDatabase(path, false, error, [&](QSqlDatabase &db) {
        if (!db.transaction()) { if (error) *error = db.lastError().text(); return false; }
        QSqlQuery query(db);
        const QStringList statements = version == 1 ? QStringList{
            QStringLiteral("CREATE TABLE pipeline_setting (module_id TEXT PRIMARY KEY, execution_mode TEXT NOT NULL CHECK(execution_mode IN ('AUTO','REVIEW','MANUAL','SKIP')))")
        } : QStringList{
            QStringLiteral("CREATE TABLE module_run (run_id TEXT PRIMARY KEY, inspection_id TEXT REFERENCES inspection(inspection_id), module_id TEXT NOT NULL, status TEXT NOT NULL, started_at TEXT NOT NULL, finished_at TEXT, error_message TEXT NOT NULL DEFAULT '')"),
            QStringLiteral("CREATE INDEX module_run_scope ON module_run(inspection_id,module_id,started_at)"),
            QStringLiteral("CREATE TABLE artifact (artifact_id TEXT PRIMARY KEY, created_by_run_id TEXT NOT NULL REFERENCES module_run(run_id), artifact_type TEXT NOT NULL, storage_path TEXT NOT NULL, status TEXT NOT NULL, created_at TEXT NOT NULL, is_active INTEGER NOT NULL DEFAULT 0 CHECK(is_active IN (0,1)))"),
            QStringLiteral("CREATE TABLE artifact_dependency (parent_artifact_id TEXT NOT NULL REFERENCES artifact(artifact_id), child_artifact_id TEXT NOT NULL REFERENCES artifact(artifact_id), PRIMARY KEY(parent_artifact_id,child_artifact_id))")
        };
        bool ok = true;
        for (const auto &sql : statements) if (!query.exec(sql)) { ok = false; break; }
        if (ok) {
            query.prepare(QStringLiteral("INSERT INTO schema_version VALUES(?,?,?,?)"));
            query.addBindValue(version + 1);
            query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            query.addBindValue(QStringLiteral("0.1.0"));
            query.addBindValue(QStringLiteral("bootstrap-v%1").arg(version + 1));
            ok = query.exec();
        }
        if (ok && db.commit()) return true;
        if (error) *error = ok ? db.lastError().text() : query.lastError().text();
        db.rollback();
        return false;
    });
    return migrated && ensureManagerSchema(path, error);
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
