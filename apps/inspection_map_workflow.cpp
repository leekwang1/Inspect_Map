#include "inspection_map_workflow.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuaternion>
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
                "area_mm2 REAL NOT NULL)"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS idx_map_projection_item_run "
                "ON map_projection_item(run_id,part_id)"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS map_projection_review ("
                "run_id TEXT NOT NULL REFERENCES map_projection_run(run_id), "
                "object_id TEXT NOT NULL REFERENCES damage_object(object_id), "
                "decision TEXT NOT NULL, part_id TEXT NOT NULL DEFAULT '', face_id TEXT NOT NULL DEFAULT '', "
                "drawing_id TEXT NOT NULL DEFAULT '', paths_json TEXT NOT NULL DEFAULT '[]', "
                "length_mm REAL NOT NULL DEFAULT 0, area_mm2 REAL NOT NULL DEFAULT 0, "
                "updated_at TEXT NOT NULL, PRIMARY KEY(run_id,object_id))")
        };
        for (const auto &statement : statements) if (!query.exec(statement)) {
            if (error) *error = query.lastError().text(); return false;
        }
        for (const auto &column : {QStringLiteral("box_width_mm"), QStringLiteral("box_length_mm"),
                                    QStringLiteral("crack_p90_mm")}) {
            if (db.record(QStringLiteral("map_projection_item")).indexOf(column) >= 0) continue;
            if (!query.exec(QStringLiteral("ALTER TABLE map_projection_item ADD COLUMN %1 REAL NOT NULL DEFAULT 0")
                            .arg(column))) {
                if (error) *error = query.lastError().text(); return false;
            }
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
        reviews.prepare(QStringLiteral("SELECT object_id,decision,part_id,face_id,drawing_id,paths_json,"
            "length_mm,area_mm2 FROM map_projection_review WHERE run_id=?"));
        reviews.addBindValue(batch->runId);
        if (!exec(reviews, error)) return false;
        while (reviews.next()) {
            const int index = byObject.value(reviews.value(0).toString(), -1);
            if (index < 0) continue;
            auto &item = batch->items[index];
            item.reviewState = reviews.value(1).toString();
            if (item.reviewState == QStringLiteral("CORRECTED")) {
                item.partId = reviews.value(2).toString(); item.faceId = reviews.value(3).toString();
                item.drawingId = reviews.value(4).toString();
                item.paths = readPaths(reviews.value(5).toByteArray());
                item.lengthMm = reviews.value(6).toDouble(); item.areaMm2 = reviews.value(7).toDouble();
                item.boxWidthMm = 0; item.boxLengthMm = 0;
                item.status = QStringLiteral("PROJECTED"); item.reason = QStringLiteral("수동 위치·물량 확정");
            } else if (item.reviewState == QStringLiteral("EXCLUDED")) {
                item.status = QStringLiteral("EXCLUDED"); item.reason = QStringLiteral("망도 검토 제외");
                item.lengthMm = 0; item.areaMm2 = 0;
            }
        }
        return true;
    });
}

bool InspectionMapWorkflow::review(const QString &projectDb, const QString &runId,
                                   const QString &objectId, const InspectionMapEngine &engine,
                                   const QString &decision,
                                   const ProjectedDamage *corrected, QString *error) {
    if (runId.isEmpty() || objectId.isEmpty() ||
        !QStringList{QStringLiteral("RESET"), QStringLiteral("EXCLUDED"),
                     QStringLiteral("DEFERRED"), QStringLiteral("CORRECTED")}.contains(decision)) {
        if (error) *error = QStringLiteral("검토 대상 또는 판정이 유효하지 않습니다."); return false;
    }
    if (decision == QStringLiteral("CORRECTED") &&
        (!corrected || corrected->partId.isEmpty() || corrected->faceId.isEmpty() ||
         corrected->drawingId.isEmpty() || corrected->paths.isEmpty() ||
         (corrected->lengthMm <= 0 && corrected->areaMm2 <= 0))) {
        if (error) *error = QStringLiteral("확정할 3D 형상과 물량이 필요합니다."); return false;
    }
    if (decision == QStringLiteral("CORRECTED")) {
        const auto *drawing = engine.drawing(corrected->drawingId);
        if (!drawing || drawing->partId != corrected->partId ||
            !drawing->faceIds.contains(corrected->faceId) || corrected->paths.size() != 1) {
            if (error) *error = QStringLiteral("선택한 부재·도면·면이 일치하지 않습니다."); return false;
        }
        const auto &path = corrected->paths.first();
        const bool area = corrected->areaMm2 > 0;
        if (path.size() < (area ? 3 : 2) ||
            (area && corrected->lengthMm != 0) || (!area && corrected->areaMm2 != 0)) {
            if (error) *error = QStringLiteral("선 또는 영역 형식이 유효하지 않습니다."); return false;
        }
        for (const auto &world : path) {
            QVector3D recovered;
            if (!engine.mapPoint(corrected->drawingId, corrected->faceId,
                                 InspectionMapEngine::drawingPoint(*drawing, world), &recovered) ||
                (recovered-world).length() > 0.01f) {
                if (error) *error = QStringLiteral("선택한 모델 면을 벗어난 점이 있습니다."); return false;
            }
        }
        double measured = 0;
        if (area) {
            QVector3D cross;
            for (int i = 0; i < path.size(); ++i)
                cross += QVector3D::crossProduct(path[i], path[(i+1)%path.size()]);
            measured = 500000.0 * double(cross.length());
        } else for (int i = 1; i < path.size(); ++i)
            measured += 1000.0 * double((path[i]-path[i-1]).length());
        const double submitted = area ? corrected->areaMm2 : corrected->lengthMm;
        if (!std::isfinite(measured) || measured <= 0 ||
            std::abs(measured-submitted) > std::max(0.01, measured*0.0001)) {
            if (error) *error = QStringLiteral("3D 형상과 물량값이 일치하지 않습니다."); return false;
        }
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
        QSqlQuery query(db);
        if (decision == QStringLiteral("RESET")) {
            query.prepare(QStringLiteral("DELETE FROM map_projection_review WHERE run_id=? AND object_id=?"));
            query.addBindValue(runId); query.addBindValue(objectId);
        } else {
            query.prepare(QStringLiteral("INSERT INTO map_projection_review "
                "(run_id,object_id,decision,part_id,face_id,drawing_id,paths_json,length_mm,area_mm2,updated_at) "
                "VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(run_id,object_id) DO UPDATE SET "
                "decision=excluded.decision,part_id=excluded.part_id,face_id=excluded.face_id,"
                "drawing_id=excluded.drawing_id,paths_json=excluded.paths_json,"
                "length_mm=excluded.length_mm,area_mm2=excluded.area_mm2,updated_at=excluded.updated_at"));
            query.addBindValue(runId); query.addBindValue(objectId); query.addBindValue(decision);
            query.addBindValue(corrected ? corrected->partId : QStringLiteral(""));
            query.addBindValue(corrected ? corrected->faceId : QStringLiteral(""));
            query.addBindValue(corrected ? corrected->drawingId : QStringLiteral(""));
            query.addBindValue(corrected ? json(pathsJson(corrected->paths)) : QStringLiteral("[]"));
            query.addBindValue(corrected ? corrected->lengthMm : 0.0);
            query.addBindValue(corrected ? corrected->areaMm2 : 0.0);
            query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        }
        return exec(query, error);
    });
}
