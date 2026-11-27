#include "inspection_map_workflow.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuaternion>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace {
bool withDb(const QString &path, QString *error, const std::function<bool(QSqlDatabase &)> &body) {
    const QString name = QUuid::createUuid().toString(QUuid::WithoutBraces);
    bool ok = false;
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        db.setDatabaseName(path);
        if (!db.open()) { if (error) *error = db.lastError().text(); }
        else { ok = body(db); db.close(); }
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}

bool exec(QSqlQuery &query, QString *error) {
    if (query.exec()) return true;
    if (error) *error = query.lastError().text();
    return false;
}

QString json(const QJsonValue &value) {
    if (value.isObject()) return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
}

QString nonNull(const QString &value) { return value.isNull() ? QStringLiteral("") : value; }

QJsonArray pathsJson(const QVector<QVector<QVector3D>> &paths) {
    QJsonArray outer;
    for (const auto &path : paths) {
        QJsonArray line;
        for (const auto &point : path) line.append(QJsonArray{point.x(), point.y(), point.z()});
        outer.append(line);
    }
    return outer;
}

QVector<QVector<QVector3D>> readPaths(const QByteArray &text) {
    QVector<QVector<QVector3D>> paths;
    for (const auto &lineValue : QJsonDocument::fromJson(text).array()) {
        QVector<QVector3D> line;
        for (const auto &pointValue : lineValue.toArray()) {
            const auto point = pointValue.toArray();
            if (point.size() == 3)
                line.append(QVector3D(float(point[0].toDouble()), float(point[1].toDouble()),
                                      float(point[2].toDouble())));
        }
        if (!line.isEmpty()) paths.append(line);
    }
    return paths;
}

struct Camera {
    bool valid = false;
    QVector3D origin;
    QQuaternion orientation;
    double fx = 0, fy = 0, cx = 0, cy = 0;
    QString reason;
};

Camera camera(QSqlDatabase &db, const QString &projectDb, const QString &imageId,
              const QString &registrationId) {
    Camera result;
    QSqlQuery pose(db);
    pose.prepare(QStringLiteral("SELECT pose_json FROM camera_pose WHERE image_id=? "
        "AND pose_stage='MODEL_ALIGNED' AND status='VALID'"));
    pose.addBindValue(imageId);
    if (!pose.exec() || !pose.next()) {
        result.reason = QStringLiteral("모델 좌표계의 카메라 pose 없음"); return result;
    }
    const auto poseObject = QJsonDocument::fromJson(pose.value(0).toByteArray()).object();
    if (poseObject.value(QStringLiteral("registration_id")).toString() != registrationId) {
        result.reason = QStringLiteral("카메라 pose가 현재 정합 결과와 다름"); return result;
    }
    const auto position = poseObject.value(QStringLiteral("position")).toObject();
    const auto orientation = poseObject.value(QStringLiteral("orientation")).toObject();
    result.origin = QVector3D(float(position.value(QStringLiteral("x")).toDouble()),
                              float(position.value(QStringLiteral("y")).toDouble()),
                              float(position.value(QStringLiteral("z")).toDouble()));
    result.orientation = QQuaternion(float(orientation.value(QStringLiteral("w")).toDouble()),
                                     float(orientation.value(QStringLiteral("x")).toDouble()),
                                     float(orientation.value(QStringLiteral("y")).toDouble()),
                                     float(orientation.value(QStringLiteral("z")).toDouble())).normalized();
    QSqlQuery variant(db);
    variant.prepare(QStringLiteral("SELECT calibration_sha256 FROM image_variant WHERE image_id=? "
        "AND variant_type!='RAW' AND status='VALID' ORDER BY is_active DESC LIMIT 1"));
    variant.addBindValue(imageId);
    if (!variant.exec() || !variant.next() || variant.value(0).toString().isEmpty()) {
        result.reason = QStringLiteral("보정 영상 또는 카메라 행렬 없음"); return result;
    }
    QFile calibration(QDir(QFileInfo(projectDb).absolutePath()).filePath(
        QStringLiteral("source/calibration/") + variant.value(0).toString() + QStringLiteral(".json")));
    if (!calibration.open(QIODevice::ReadOnly)) {
        result.reason = QStringLiteral("보정 행렬 파일 없음"); return result;
    }
    const auto matrix = QJsonDocument::fromJson(calibration.readAll()).object().value(QStringLiteral("P")).toArray();
    if (matrix.size() < 2 || matrix[0].toArray().size() < 3 || matrix[1].toArray().size() < 3) {
        result.reason = QStringLiteral("보정 행렬 P가 유효하지 않음"); return result;
    }
    result.fx = matrix[0].toArray()[0].toDouble(); result.cx = matrix[0].toArray()[2].toDouble();
    result.fy = matrix[1].toArray()[1].toDouble(); result.cy = matrix[1].toArray()[2].toDouble();
    result.valid = std::isfinite(result.fx) && std::isfinite(result.fy) &&
        result.fx > 0 && result.fy > 0 && !result.orientation.isNull();
    if (!result.valid) result.reason = QStringLiteral("카메라 행렬 또는 자세가 유효하지 않음");
    return result;
}

struct SamplePath { QVector<QPointF> pixels; bool polygon = false; };
QVector<SamplePath> samplePaths(const QJsonObject &geometry, const QJsonArray &bbox) {
    QVector<SamplePath> result;
    auto add = [&](const QJsonArray &source, bool polygon) {
        SamplePath path;
        path.polygon = polygon;
        const int stride = std::max(1, int((source.size() + 255) / 256));
        for (int i = 0; i < source.size(); i += stride) {
            const auto point = source[i].toArray();
            if (point.size() >= 2) path.pixels.append(QPointF(point[0].toDouble(), point[1].toDouble()));
        }
        if (!source.isEmpty() && (source.size() - 1) % stride != 0) {
            const auto point = source.last().toArray();
            if (point.size() >= 2) path.pixels.append(QPointF(point[0].toDouble(), point[1].toDouble()));
        }
        if (!path.pixels.isEmpty()) result.append(path);
    };
    for (const auto &value : geometry.value(QStringLiteral("lines")).toArray()) add(value.toArray(), false);
    for (const auto &value : geometry.value(QStringLiteral("polygon")).toArray()) add(value.toArray(), true);
    if (result.isEmpty() && bbox.size() == 4)
        result.append({{QPointF((bbox[0].toDouble() + bbox[2].toDouble()) / 2,
                                (bbox[1].toDouble() + bbox[3].toDouble()) / 2)}, false});
    return result;
}

QVector<QPointF> boundaryPixels(const QJsonObject &geometry, const QJsonArray &bbox) {
    QVector<QPointF> pixels;
    auto appendRing = [&](const QJsonArray &ring) {
        const int stride = std::max(1, int((ring.size() + 255) / 256));
        for (int i = 0; i < ring.size(); i += stride) {
            const auto point = ring[i].toArray();
            if (point.size() >= 2) pixels.append(QPointF(point[0].toDouble(), point[1].toDouble()));
        }
    };
    for (const auto &region : geometry.value(QStringLiteral("regions")).toArray())
        for (const auto &ring : region.toObject().value(QStringLiteral("polygon")).toArray())
            appendRing(ring.toArray());
    if (pixels.isEmpty())
        for (const auto &ring : geometry.value(QStringLiteral("polygon")).toArray())
            appendRing(ring.toArray());
    if (pixels.isEmpty() && bbox.size() == 4) {
        const double x1 = bbox[0].toDouble(), y1 = bbox[1].toDouble();
        const double x2 = bbox[2].toDouble(), y2 = bbox[3].toDouble();
        pixels = {{x1, y1}, {x2, y1}, {x2, y2}, {x1, y2}};
    }
    return pixels;
}

void projectedBox(const InspectionMapEngine &engine, const Camera &cameraData,
                  const QJsonObject &geometry, const QJsonArray &bbox,
                  const QString &faceId, double *widthMm, double *lengthMm) {
    const auto pixels = boundaryPixels(geometry, bbox);
    QVector<QVector3D> points;
    for (const auto &pixel : pixels) {
        const auto ray = InspectionMapEngine::cameraRay(
            QVector3D(float(pixel.x()), float(pixel.y()), 0), cameraData.fx, cameraData.fy,
            cameraData.cx, cameraData.cy, cameraData.orientation);
        const auto hit = engine.cast(cameraData.origin, ray);
        if (hit.found && hit.faceId == faceId) points.append(hit.point);
    }
    if (points.size() < 3 || points.size() * 10 < pixels.size() * 9) return;
    const QVector3D origin = points.first();
    QVector3D u;
    for (const auto &point : points)
        if ((point-origin).lengthSquared() > u.lengthSquared()) u = point-origin;
    if (u.lengthSquared() < 1e-12f) return;
    u.normalize();
    QVector3D normal;
    for (const auto &point : points) {
        const auto candidate = QVector3D::crossProduct(u, point-origin);
        if (candidate.lengthSquared() > normal.lengthSquared()) normal = candidate;
    }
    if (normal.lengthSquared() < 1e-12f) return;
    normal.normalize();
    double span = 0;
    for (const auto &point : points)
        span = std::max(span, double((point-origin).length()));
    double maximumOffset = 0;
    for (const auto &point : points)
        maximumOffset = std::max(maximumOffset,
            std::abs(double(QVector3D::dotProduct(point-origin, normal))));
    if (maximumOffset > std::max(0.005, span * 0.02)) return;
    const QVector3D v = QVector3D::crossProduct(normal, u);
    QVector<QPointF> flat;
    for (const auto &point : points)
        flat.append(QPointF(QVector3D::dotProduct(point-origin, u),
                            QVector3D::dotProduct(point-origin, v)));
    std::sort(flat.begin(), flat.end(), [](const auto &a, const auto &b) {
        return a.x() == b.x() ? a.y() < b.y() : a.x() < b.x();
    });
    auto cross = [](const QPointF &a, const QPointF &b, const QPointF &c) {
        return (b.x()-a.x()) * (c.y()-a.y()) - (b.y()-a.y()) * (c.x()-a.x());
    };
    QVector<QPointF> hull;
    for (const auto &point : flat) {
        while (hull.size() > 1 && cross(hull[hull.size()-2], hull.last(), point) <= 0) hull.removeLast();
        hull.append(point);
    }
    const int lower = hull.size();
    for (int i = flat.size()-2; i >= 0; --i) {
        const auto &point = flat[i];
        while (hull.size() > lower && cross(hull[hull.size()-2], hull.last(), point) <= 0) hull.removeLast();
        hull.append(point);
    }
    if (hull.size() > 1) hull.removeLast();
    if (hull.size() < 3) return;
    double bestArea = std::numeric_limits<double>::infinity();
    for (int i = 0; i < hull.size(); ++i) {
        const QPointF edge = hull[(i+1) % hull.size()] - hull[i];
        const double size = std::hypot(edge.x(), edge.y());
        if (size <= 1e-9) continue;
        const QPointF axis(edge.x()/size, edge.y()/size);
        double minX = std::numeric_limits<double>::infinity(), minY = minX;
        double maxX = -minX, maxY = -minX;
        for (const auto &point : hull) {
            const double x = point.x()*axis.x()+point.y()*axis.y();
            const double y = -point.x()*axis.y()+point.y()*axis.x();
            minX = std::min(minX, x); maxX = std::max(maxX, x);
            minY = std::min(minY, y); maxY = std::max(maxY, y);
        }
        const double dx = maxX-minX, dy = maxY-minY;
        if (dx*dy < bestArea) {
            bestArea = dx*dy;
            *widthMm = 1000.0 * std::min(dx, dy);
            *lengthMm = 1000.0 * std::max(dx, dy);
        }
    }
}

ProjectedDamage project(const InspectionMapEngine &engine, const Camera &cameraData,
                        const QJsonObject &geometry, const QJsonArray &bbox,
                        const QJsonObject &measurements, ProjectedDamage item) {
    if (!cameraData.valid) {
        item.status = QStringLiteral("UNAVAILABLE"); item.reason = cameraData.reason; return item;
    }
    const auto sourcePaths = samplePaths(geometry, bbox);
    if (sourcePaths.isEmpty()) {
        item.status = QStringLiteral("NEEDS_REVIEW"); item.reason = QStringLiteral("투영할 형상 없음"); return item;
    }
    QVector<QVector<MapHit>> hitPaths;
    QHash<QString, int> votes;
    int total = 0, found = 0;
    for (const auto &path : sourcePaths) {
        QVector<MapHit> hits;
        for (const auto &pixel : path.pixels) {
            const auto ray = InspectionMapEngine::cameraRay(
                QVector3D(float(pixel.x()), float(pixel.y()), 0), cameraData.fx, cameraData.fy,
                cameraData.cx, cameraData.cy, cameraData.orientation);
            const auto hit = engine.cast(cameraData.origin, ray);
            hits.append(hit);
            ++total;
            if (hit.found) { ++found; ++votes[hit.faceId]; }
        }
        hitPaths.append(hits);
    }
    if (votes.isEmpty()) {
        item.status = QStringLiteral("NEEDS_REVIEW"); item.reason = QStringLiteral("모델 표면과 교차하지 않음");
        return item;
    }
    QString winningFace;
    int winningVotes = 0;
    for (auto it = votes.cbegin(); it != votes.cend(); ++it)
        if (it.value() > winningVotes) { winningFace = it.key(); winningVotes = it.value(); }
    for (const auto &path : hitPaths) for (const auto &hit : path)
        if (hit.found && hit.faceId == winningFace) { item.partId = hit.partId; break; }
    item.faceId = winningFace;
    const auto *drawing = engine.drawingForFace(winningFace);
    if (drawing) item.drawingId = drawing->id;
    const bool singlePoint = total == 1;
    const bool coherent = found >= std::ceil(total * 0.9) && winningVotes >= std::ceil(found * 0.9);
    item.status = coherent && !singlePoint && drawing ? QStringLiteral("PROJECTED") : QStringLiteral("NEEDS_REVIEW");
    if (!coherent) item.reason = QStringLiteral("투영 누락 또는 여러 면에 걸침");
    else if (singlePoint) item.reason = QStringLiteral("bbox 중심점만 투영됨: 형상 확인 필요");
    else if (!drawing) item.reason = QStringLiteral("면에 대응하는 도면 없음");
    bool incompletePolygon = false;
    bool hasPolygon = false;
    for (int p = 0; p < hitPaths.size(); ++p) {
        QVector<QVector3D> line;
        for (const auto &hit : hitPaths[p]) {
            if (!hit.found || hit.faceId != winningFace) {
                if (!line.isEmpty()) item.paths.append(line);
                line.clear();
                continue;
            }
            if (!line.isEmpty() && !sourcePaths[p].polygon)
                item.lengthMm += 1000.0 * double((hit.point - line.last()).length());
            line.append(hit.point);
        }
        if (!line.isEmpty()) item.paths.append(line);
        if (sourcePaths[p].polygon) hasPolygon = true;
        if (sourcePaths[p].polygon && line.size() >= 3 && line.size() == hitPaths[p].size()) {
            QVector3D cross;
            for (int i = 0; i < line.size(); ++i)
                cross += QVector3D::crossProduct(line[i], line[(i + 1) % line.size()]);
            item.areaMm2 += 500000.0 * double(cross.length());
        } else if (sourcePaths[p].polygon) incompletePolygon = true;
    }
    if (item.status == QStringLiteral("PROJECTED") &&
        (incompletePolygon || (hasPolygon && item.areaMm2 <= 0) ||
         (!hasPolygon && item.lengthMm <= 0))) {
        item.status = QStringLiteral("NEEDS_REVIEW");
        item.reason = QStringLiteral("형상 전체가 한 면에 투영되지 않아 물량 계산 불가");
    }
    if (item.status != QStringLiteral("PROJECTED")) {
        item.lengthMm = 0;
        item.areaMm2 = 0;
    } else {
        projectedBox(engine, cameraData, geometry, bbox, winningFace,
                     &item.boxWidthMm, &item.boxLengthMm);
        if (item.type == QStringLiteral("crack") &&
            measurements.value(QStringLiteral("width_resolvable")).toBool()) {
            const double p90 = measurements.value(QStringLiteral("width_p90"))
                .toObject().value(QStringLiteral("mm")).toDouble();
            if (std::isfinite(p90) && p90 > 0) item.crackP90Mm = p90;
        }
    }
    return item;
}
}

bool InspectionMapWorkflow::ensureSchema(const QString &projectDb, QString *error) {
    return withDb(projectDb, error, [&](QSqlDatabase &db) {
        QSqlQuery query(db);
        const QStringList statements{
            QStringLiteral("CREATE TABLE IF NOT EXISTS map_projection_run ("
                "run_id TEXT PRIMARY KEY, inspection_id TEXT NOT NULL, dataset_id TEXT NOT NULL, "
                "registration_id TEXT NOT NULL, created_at TEXT NOT NULL, active INTEGER NOT NULL DEFAULT 1)"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS map_projection_item ("
                "item_id TEXT PRIMARY KEY, run_id TEXT NOT NULL REFERENCES map_projection_run(run_id), "
                "object_id TEXT NOT NULL REFERENCES damage_object(object_id), image_id TEXT NOT NULL, "
                "image_name TEXT NOT NULL, type TEXT NOT NULL, part_id TEXT NOT NULL, "
                "face_id TEXT NOT NULL, drawing_id TEXT NOT NULL, status TEXT NOT NULL, "
                "reason TEXT NOT NULL, paths_json TEXT NOT NULL, length_mm REAL NOT NULL, "
                "area_mm2 REAL NOT NULL, box_width_mm REAL NOT NULL DEFAULT 0, "
                "box_length_mm REAL NOT NULL DEFAULT 0, crack_p90_mm REAL NOT NULL DEFAULT 0)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_map_projection_item_run "
                "ON map_projection_item(run_id,part_id)"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS map_projection_review ("
                "run_id TEXT NOT NULL REFERENCES map_projection_run(run_id), "
                "object_id TEXT NOT NULL REFERENCES damage_object(object_id), "
                "decision TEXT NOT NULL CHECK(decision IN ('EXCLUDED','DEFERRED')), "
                "updated_at TEXT NOT NULL, PRIMARY KEY(run_id,object_id))"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS map_damage_group ("
                "group_id TEXT PRIMARY KEY, run_id TEXT NOT NULL REFERENCES map_projection_run(run_id), "
                "kind TEXT NOT NULL CHECK(kind IN ('DUPLICATE','CONNECTED')), "
                "representative_id TEXT NOT NULL, created_at TEXT NOT NULL)"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS map_damage_group_member ("
                "group_id TEXT NOT NULL REFERENCES map_damage_group(group_id), "
                "run_id TEXT NOT NULL, object_id TEXT NOT NULL, "
                "PRIMARY KEY(run_id,object_id))")
        };
        for (const auto &statement : statements) if (!query.exec(statement)) {
            if (error) *error = query.lastError().text(); return false;
        }
        return true;
    });
}

bool InspectionMapWorkflow::run(const QString &projectDb, const QString &inspectionId,
                                const QString &datasetId, const InspectionMapEngine &engine,
                                ProjectionBatch *batch, QString *error) {
    if (!batch || inspectionId.isEmpty() || datasetId.isEmpty()) {
        if (error) *error = QStringLiteral("점검과 데이터 세트가 필요합니다."); return false;
    }
    if (!ensureSchema(projectDb, error)) return false;
    return withDb(projectDb, error, [&](QSqlDatabase &db) {
        QSqlQuery registration(db);
        registration.prepare(QStringLiteral("SELECT registration_id FROM registration_result "
            "WHERE inspection_id=? AND active=1 AND review_status='CONFIRMED' "
            "ORDER BY confirmed_at DESC LIMIT 1"));
        registration.addBindValue(inspectionId);
        if (!exec(registration, error)) return false;
        if (!registration.next()) {
            if (error) *error = QStringLiteral("확정된 정합 결과가 없습니다."); return false;
        }
        ProjectionBatch next;
        next.runId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        next.registrationId = registration.value(0).toString();
        QSqlQuery objects(db);
        objects.prepare(QStringLiteral("SELECT o.object_id,r.image_id,COALESCE(i.display_name,i.source_identifier),"
            "o.type,o.geometry_json,o.bbox_json,o.measurements_json FROM damage_object o "
            "JOIN damage_result r ON r.result_id=o.result_id JOIN image i ON i.image_id=r.image_id "
            "WHERE r.active=1 AND r.dataset_id=? AND o.review_state='ACCEPTED' "
            "AND o.review_confirmed=1 ORDER BY i.capture_time,i.display_name,o.source_id"));
        objects.addBindValue(datasetId);
        if (!exec(objects, error)) return false;
        QHash<QString, Camera> cameras;
        while (objects.next()) {
            ProjectedDamage item;
            item.objectId = objects.value(0).toString();
            item.imageId = objects.value(1).toString();
            item.imageName = objects.value(2).toString();
            item.type = objects.value(3).toString();
            if (!cameras.contains(item.imageId))
                cameras.insert(item.imageId, camera(db, projectDb, item.imageId, next.registrationId));
            next.items.append(project(engine, cameras.value(item.imageId),
                QJsonDocument::fromJson(objects.value(4).toByteArray()).object(),
                QJsonDocument::fromJson(objects.value(5).toByteArray()).array(),
                QJsonDocument::fromJson(objects.value(6).toByteArray()).object(), item));
        }
        objects.finish();
        if (!db.transaction()) { if (error) *error = db.lastError().text(); return false; }
        QSqlQuery deactivate(db);
        deactivate.prepare(QStringLiteral("UPDATE map_projection_run SET active=0 "
            "WHERE inspection_id=? AND dataset_id=? AND active=1"));
        deactivate.addBindValue(inspectionId); deactivate.addBindValue(datasetId);
        if (!exec(deactivate, error)) { db.rollback(); return false; }
        QSqlQuery insertRun(db);
        insertRun.prepare(QStringLiteral("INSERT INTO map_projection_run VALUES(?,?,?,?,?,1)"));
        insertRun.addBindValue(next.runId); insertRun.addBindValue(inspectionId);
        insertRun.addBindValue(datasetId); insertRun.addBindValue(next.registrationId);
        insertRun.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        if (!exec(insertRun, error)) { db.rollback(); return false; }
        QSqlQuery insert(db);
        insert.prepare(QStringLiteral("INSERT INTO map_projection_item "
            "(item_id,run_id,object_id,image_id,image_name,type,part_id,face_id,drawing_id,status,"
            "reason,paths_json,length_mm,area_mm2,box_width_mm,box_length_mm,crack_p90_mm) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
        for (const auto &item : next.items) {
            const QVariantList values{QUuid::createUuid().toString(QUuid::WithoutBraces), next.runId,
                item.objectId, item.imageId, item.imageName, item.type, nonNull(item.partId),
                nonNull(item.faceId), nonNull(item.drawingId), item.status, nonNull(item.reason),
                json(pathsJson(item.paths)),
                item.lengthMm, item.areaMm2, item.boxWidthMm, item.boxLengthMm, item.crackP90Mm};
            for (int i = 0; i < values.size(); ++i) insert.bindValue(i, values[i]);
            if (!exec(insert, error)) { db.rollback(); return false; }
        }
        if (!db.commit()) { if (error) *error = db.lastError().text(); db.rollback(); return false; }
        *batch = std::move(next);
        return true;
    });
}

bool InspectionMapWorkflow::active(const QString &projectDb, const QString &inspectionId, const QString &datasetId,
                                   ProjectionBatch *batch, QString *error) {
    if (!batch) return false;
    *batch = {};
    if (!ensureSchema(projectDb, error)) return false;
    return withDb(projectDb, error, [&](QSqlDatabase &db) {
        QSqlQuery run(db);
        run.prepare(QStringLiteral("SELECT run_id,registration_id FROM map_projection_run "
            "WHERE inspection_id=? AND dataset_id=? AND active=1 ORDER BY created_at DESC LIMIT 1"));
        run.addBindValue(inspectionId);
        run.addBindValue(datasetId);
        if (!exec(run, error)) return false;
        if (!run.next()) return true;
        batch->runId = run.value(0).toString(); batch->registrationId = run.value(1).toString();
        QSqlQuery items(db);
        items.prepare(QStringLiteral("SELECT object_id,image_id,image_name,type,part_id,face_id,"
            "drawing_id,status,reason,paths_json,length_mm,area_mm2,"
            "box_width_mm,box_length_mm,crack_p90_mm FROM map_projection_item WHERE run_id=? "
            "ORDER BY image_name,object_id"));
        items.addBindValue(batch->runId);
        if (!exec(items, error)) return false;
        while (items.next()) {
            batch->items.append({items.value(0).toString(), items.value(1).toString(),
                items.value(2).toString(), items.value(3).toString(), items.value(4).toString(),
                items.value(5).toString(), items.value(6).toString(), items.value(7).toString(),
                items.value(8).toString(), {}, readPaths(items.value(9).toByteArray()),
                items.value(10).toDouble(), items.value(11).toDouble()});
            auto &loaded = batch->items.last();
            loaded.boxWidthMm = items.value(12).toDouble();
            loaded.boxLengthMm = items.value(13).toDouble();
            loaded.crackP90Mm = items.value(14).toDouble();
        }
        items.finish();
        QHash<QString, int> byObject;
        for (int i = 0; i < batch->items.size(); ++i) byObject.insert(batch->items[i].objectId, i);
        QSqlQuery reviews(db);
        reviews.prepare(QStringLiteral("SELECT object_id,decision FROM map_projection_review WHERE run_id=?"));
        reviews.addBindValue(batch->runId);
        if (!exec(reviews, error)) return false;
        while (reviews.next()) {
            const int index = byObject.value(reviews.value(0).toString(), -1);
            if (index < 0) continue;
            auto &item = batch->items[index];
            item.reviewState = reviews.value(1).toString();
            if (item.reviewState == QStringLiteral("EXCLUDED")) {
                item.status = QStringLiteral("EXCLUDED"); item.reason = QStringLiteral("망도 검토 제외");
                item.lengthMm = 0; item.areaMm2 = 0;
            }
        }
        reviews.finish();
        QSet<QString> hidden;
        QHash<QString, QStringList> connectedMembers;
        QSqlQuery members(db);
        members.prepare(QStringLiteral("SELECT g.group_id,g.kind,g.representative_id,m.object_id "
            "FROM map_damage_group g JOIN map_damage_group_member m ON m.group_id=g.group_id "
            "WHERE g.run_id=? ORDER BY g.group_id,m.object_id"));
        members.addBindValue(batch->runId);
        if (!exec(members, error)) return false;
        while (members.next()) {
            const QString representativeId = members.value(2).toString();
            const QString objectId = members.value(3).toString();
            const int representative = byObject.value(representativeId, -1);
            const int source = byObject.value(objectId, -1);
            if (representative < 0 || source < 0) continue;
            auto &item = batch->items[representative];
            item.groupId = members.value(0).toString();
            item.groupKind = members.value(1).toString();
            item.sourceObjectIds.append(objectId);
            item.sourceImageNames.append(batch->items[source].imageName);
            if (item.groupKind == QStringLiteral("CONNECTED"))
                connectedMembers[representativeId].append(objectId);
            if (objectId != representativeId) hidden.insert(objectId);
        }
        members.finish();
        for (auto it = connectedMembers.cbegin(); it != connectedMembers.cend(); ++it) {
            const int representative = byObject.value(it.key(), -1);
            if (representative < 0) continue;
            QVector<QVector<QVector3D>> paths;
            double lengthMm = 0, boxLengthMm = 0, boxWidthMm = 0, crackP90Mm = 0;
            for (const auto &id : it.value()) {
                const int source = byObject.value(id, -1);
                if (source < 0) continue;
                const auto &member = batch->items[source];
                paths += member.paths;
                lengthMm += member.lengthMm;
                boxLengthMm += member.boxLengthMm;
                boxWidthMm = std::max(boxWidthMm, member.boxWidthMm);
                crackP90Mm = std::max(crackP90Mm, member.crackP90Mm);
            }
            auto &item = batch->items[representative];
            item.paths = std::move(paths);
            item.lengthMm = lengthMm;
            item.boxLengthMm = boxLengthMm;
            item.boxWidthMm = boxWidthMm;
            item.crackP90Mm = crackP90Mm;
        }
        for (int i = batch->items.size()-1; i >= 0; --i)
            if (hidden.contains(batch->items[i].objectId)) batch->items.removeAt(i);
        return true;
    });
}

bool InspectionMapWorkflow::exportModelDamageJson(const ProjectionBatch &batch,
        const QString &inspectionId, const QString &datasetId, const QString &path,
        int *exported, QString *error) {
    if (exported) *exported = 0;
    if (batch.runId.isEmpty() || inspectionId.isEmpty() || datasetId.isEmpty() || path.isEmpty()) {
        if (error) *error = QStringLiteral("활성 투영 결과와 출력 경로가 필요합니다.");
        return false;
    }

    QJsonArray damages;
    int excluded = 0, deferred = 0, unprojected = 0;
    for (const auto &item : batch.items) {
        if (item.status == QStringLiteral("EXCLUDED")) { ++excluded; continue; }
        if (item.reviewState == QStringLiteral("DEFERRED")) { ++deferred; continue; }
        if (item.status != QStringLiteral("PROJECTED") || item.paths.isEmpty()) {
            ++unprojected; continue;
        }
        const QStringList sourceIds = item.sourceObjectIds.isEmpty() ?
            QStringList{item.objectId} : item.sourceObjectIds;
        const QStringList sourceImages = item.sourceImageNames.isEmpty() ?
            QStringList{item.imageName} : item.sourceImageNames;
        QJsonArray sourceObjectIds, sourceImageNames;
        for (const auto &id : sourceIds) sourceObjectIds.append(id);
        for (const auto &name : sourceImages) sourceImageNames.append(name);
        auto measured = [](double millimetres, double scale) -> QJsonValue {
            return std::isfinite(millimetres) && millimetres > 0 ?
                QJsonValue(millimetres * scale) : QJsonValue(QJsonValue::Null);
        };
        damages.append(QJsonObject{
            {QStringLiteral("damage_id"), item.objectId},
            {QStringLiteral("type"), item.type},
            {QStringLiteral("part_id"), item.partId},
            {QStringLiteral("face_id"), item.faceId},
            {QStringLiteral("geometry"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("MultiPath3D")},
                {QStringLiteral("paths_xyz"), pathsJson(item.paths)}
            }},
            {QStringLiteral("measurements"), QJsonObject{
                {QStringLiteral("bbox_width_m"), measured(item.boxWidthMm, 0.001)},
                {QStringLiteral("bbox_length_m"), measured(item.boxLengthMm, 0.001)},
                {QStringLiteral("crack_width_p90_mm"), measured(item.crackP90Mm, 1.0)}
            }},
            {QStringLiteral("source"), QJsonObject{
                {QStringLiteral("group_id"), item.groupId.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(item.groupId)},
                {QStringLiteral("group_kind"), item.groupKind.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(item.groupKind)},
                {QStringLiteral("object_ids"), sourceObjectIds},
                {QStringLiteral("image_names"), sourceImageNames}
            }}
        });
    }
    const QJsonObject document{
        {QStringLiteral("schema"), QStringLiteral("inspectmap.model_damage.v1")},
        {QStringLiteral("generated_at_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("coordinate_system"), QJsonObject{
            {QStringLiteral("frame"), QStringLiteral("MODEL")},
            {QStringLiteral("unit"), QStringLiteral("m")},
            {QStringLiteral("axes"), QJsonArray{QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("z")}}
        }},
        {QStringLiteral("source"), QJsonObject{
            {QStringLiteral("inspection_id"), inspectionId},
            {QStringLiteral("dataset_id"), datasetId},
            {QStringLiteral("projection_run_id"), batch.runId},
            {QStringLiteral("registration_id"), batch.registrationId}
        }},
        {QStringLiteral("damage_count"), damages.size()},
        {QStringLiteral("omitted"), QJsonObject{
            {QStringLiteral("excluded"), excluded},
            {QStringLiteral("deferred"), deferred},
            {QStringLiteral("unprojected"), unprojected}
        }},
        {QStringLiteral("damages"), damages}
    };
    QSaveFile file(path);
    const QByteArray payload = QJsonDocument(document).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(payload) != payload.size() || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    if (exported) *exported = damages.size();
    return true;
}

bool InspectionMapWorkflow::review(const QString &projectDb, const QString &runId,
                                   const QString &objectId, const QString &decision, QString *error) {
    if (runId.isEmpty() || objectId.isEmpty() ||
        !QStringList{QStringLiteral("RESET"), QStringLiteral("EXCLUDED"),
                     QStringLiteral("DEFERRED")}.contains(decision)) {
        if (error) *error = QStringLiteral("검토 대상 또는 판정이 유효하지 않습니다."); return false;
    }
    if (!ensureSchema(projectDb, error)) return false;
    return withDb(projectDb, error, [&](QSqlDatabase &db) {
        QSqlQuery exists(db);
        exists.prepare(QStringLiteral("SELECT 1 FROM map_projection_item i JOIN map_projection_run r "
            "ON r.run_id=i.run_id WHERE i.run_id=? AND i.object_id=? AND r.active=1"));
        exists.addBindValue(runId); exists.addBindValue(objectId);
        if (!exec(exists, error)) return false;
        if (!exists.next()) { if (error) *error = QStringLiteral("현재 실행의 손상이 아닙니다."); return false; }
        exists.finish();
        QSqlQuery grouped(db);
        grouped.prepare(QStringLiteral("SELECT 1 FROM map_damage_group_member WHERE run_id=? AND object_id=?"));
        grouped.addBindValue(runId); grouped.addBindValue(objectId);
        if (!exec(grouped, error)) return false;
        if (grouped.next()) {
            if (error) *error = QStringLiteral("먼저 손상 묶음을 해제한 뒤 수정하십시오.");
            return false;
        }
        grouped.finish();
        QSqlQuery query(db);
        if (decision == QStringLiteral("RESET")) {
            query.prepare(QStringLiteral("DELETE FROM map_projection_review WHERE run_id=? AND object_id=?"));
            query.addBindValue(runId); query.addBindValue(objectId);
        } else {
            query.prepare(QStringLiteral("INSERT INTO map_projection_review "
                "(run_id,object_id,decision,updated_at) VALUES(?,?,?,?) "
                "ON CONFLICT(run_id,object_id) DO UPDATE SET "
                "decision=excluded.decision,updated_at=excluded.updated_at"));
            query.addBindValue(runId); query.addBindValue(objectId); query.addBindValue(decision);
            query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        }
        return exec(query, error);
    });
}

bool InspectionMapWorkflow::group(const QString &projectDb, const QString &runId,
                                  const QStringList &objectIds, const QString &representativeId,
                                  const QString &kind, QString *error) {
    if (runId.isEmpty() || !QStringList{QStringLiteral("DUPLICATE"),
        QStringLiteral("CONNECTED")}.contains(kind)) return false;
    const QSet<QString> selected(objectIds.cbegin(), objectIds.cend());
    if (selected.size() < 2 || !selected.contains(representativeId)) {
        if (error) *error = QStringLiteral("서로 다른 손상을 두 개 이상 선택하십시오."); return false;
    }
    if (!ensureSchema(projectDb, error)) return false;
    return withDb(projectDb, error, [&](QSqlDatabase &db) {
        QSqlQuery run(db);
        run.prepare(QStringLiteral("SELECT inspection_id,dataset_id FROM map_projection_run "
            "WHERE run_id=? AND active=1"));
        run.addBindValue(runId);
        if (!exec(run, error)) return false;
        if (!run.next()) { if (error) *error = QStringLiteral("현재 투영 결과가 아닙니다."); return false; }
        const QString inspectionId = run.value(0).toString(), datasetId = run.value(1).toString();
        run.finish();
        ProjectionBatch visible;
        if (!active(projectDb, inspectionId, datasetId, &visible, error)) return false;
        const ProjectedDamage *representative = nullptr;
        for (const auto &item : visible.items) if (item.objectId == representativeId) representative = &item;
        if (!representative || representative->status != QStringLiteral("PROJECTED") ||
            !representative->groupId.isEmpty() ||
            (kind == QStringLiteral("CONNECTED") && representative->type != QStringLiteral("crack"))) {
            if (error) *error = QStringLiteral("투영된 미묶음 균열을 대표 손상으로 선택하십시오.");
            return false;
        }
        for (const auto &id : selected) {
            const auto found = std::find_if(visible.items.cbegin(), visible.items.cend(),
                [&](const auto &item) { return item.objectId == id; });
            if (found == visible.items.cend() || found->status != QStringLiteral("PROJECTED") ||
                !found->groupId.isEmpty() || found->partId != representative->partId ||
                found->faceId != representative->faceId || found->type != representative->type ||
                (kind == QStringLiteral("CONNECTED") && found->paths.isEmpty())) {
                if (error) *error = QStringLiteral("같은 부재·모델 면·유형의 미묶음 손상만 선택할 수 있습니다.");
                return false;
            }
        }
        if (!db.transaction()) { if (error) *error = db.lastError().text(); return false; }
        const QString groupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QSqlQuery groupInsert(db);
        groupInsert.prepare(QStringLiteral("INSERT INTO map_damage_group VALUES(?,?,?,?,?)"));
        groupInsert.addBindValue(groupId); groupInsert.addBindValue(runId);
        groupInsert.addBindValue(kind); groupInsert.addBindValue(representativeId);
        groupInsert.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        if (!exec(groupInsert, error)) { db.rollback(); return false; }
        QSqlQuery memberInsert(db);
        memberInsert.prepare(QStringLiteral("INSERT INTO map_damage_group_member VALUES(?,?,?)"));
        for (const auto &id : selected) {
            memberInsert.bindValue(0, groupId); memberInsert.bindValue(1, runId);
            memberInsert.bindValue(2, id);
            if (!exec(memberInsert, error)) { db.rollback(); return false; }
        }
        if (!db.commit()) { if (error) *error = db.lastError().text(); db.rollback(); return false; }
        return true;
    });
}

bool InspectionMapWorkflow::ungroup(const QString &projectDb, const QString &runId,
                                    const QString &groupId, QString *error) {
    if (runId.isEmpty() || groupId.isEmpty()) return false;
    if (!ensureSchema(projectDb, error)) return false;
    return withDb(projectDb, error, [&](QSqlDatabase &db) {
        QSqlQuery exists(db);
        exists.prepare(QStringLiteral("SELECT 1 FROM map_damage_group g JOIN map_projection_run r "
            "ON r.run_id=g.run_id WHERE g.group_id=? AND g.run_id=? AND r.active=1"));
        exists.addBindValue(groupId); exists.addBindValue(runId);
        if (!exec(exists, error)) return false;
        if (!exists.next()) { if (error) *error = QStringLiteral("현재 손상 묶음이 아닙니다."); return false; }
        exists.finish();
        if (!db.transaction()) { if (error) *error = db.lastError().text(); return false; }
        QSqlQuery remove(db);
        remove.prepare(QStringLiteral("DELETE FROM map_damage_group_member WHERE group_id=? AND run_id=?"));
        remove.addBindValue(groupId); remove.addBindValue(runId);
        if (!exec(remove, error)) { db.rollback(); return false; }
        remove.prepare(QStringLiteral("DELETE FROM map_damage_group WHERE group_id=? AND run_id=?"));
        remove.addBindValue(groupId); remove.addBindValue(runId);
        if (!exec(remove, error)) { db.rollback(); return false; }
        if (!db.commit()) { if (error) *error = db.lastError().text(); db.rollback(); return false; }
        return true;
    });
}
