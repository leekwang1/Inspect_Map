#include "inspection_map_engine.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLineF>
#include <QTextStream>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
struct DxfPair { int code; QString value; };

struct UnrollInsert {
    QString faceId;
    QString block;
    QString direction;
    QPointF position;
    double sx = 1, sy = 1, rotation = 0;
};

QVector<QPointF> dxfVertices(const QVector<DxfPair> &pairs, int begin, int end) {
    QVector<QPointF> points;
    for (int i = begin; i < end; ++i) {
        if (pairs[i].code == 10) points.append(QPointF(pairs[i].value.toDouble(), 0));
        else if (pairs[i].code == 20 && !points.isEmpty()) points.last().setY(pairs[i].value.toDouble());
    }
    return points;
}

bool readUnrollDxf(const QString &path, QHash<QString, QVector<QPointF>> *blocks,
                   QVector<UnrollInsert> *inserts, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = file.errorString();
        return false;
    }
    QVector<DxfPair> pairs;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString codeText = stream.readLine().trimmed();
        if (stream.atEnd()) break;
        bool ok = false;
        const int code = codeText.toInt(&ok);
        const QString value = stream.readLine().trimmed();
        if (ok) pairs.append({code, value});
    }
    QString section, blockName;
    for (int i = 0; i < pairs.size();) {
        if (pairs[i].code != 0) { ++i; continue; }
        const QString type = pairs[i].value;
        int end = i + 1;
        while (end < pairs.size() && pairs[end].code != 0) ++end;
        if (type == QStringLiteral("SECTION")) {
            section.clear();
            for (int j = i + 1; j < end; ++j) if (pairs[j].code == 2) section = pairs[j].value;
        } else if (type == QStringLiteral("ENDSEC")) section.clear();
        else if (section == QStringLiteral("BLOCKS")) {
            if (type == QStringLiteral("BLOCK")) {
                blockName.clear();
                for (int j = i + 1; j < end; ++j) if (pairs[j].code == 2) { blockName = pairs[j].value; break; }
            } else if (type == QStringLiteral("ENDBLK")) blockName.clear();
            else if (type == QStringLiteral("LWPOLYLINE") && !blockName.isEmpty()) {
                auto polygon = dxfVertices(pairs, i + 1, end);
                if (polygon.size() == 4 && !blocks->contains(blockName)) blocks->insert(blockName, std::move(polygon));
            }
        } else if (section == QStringLiteral("ENTITIES") && type == QStringLiteral("INSERT")) {
            UnrollInsert insert;
            for (int j = i + 1; j < end; ++j) {
                const auto &pair = pairs[j];
                if (pair.code == 2) insert.block = pair.value;
                else if (pair.code == 10) insert.position.setX(pair.value.toDouble());
                else if (pair.code == 20) insert.position.setY(pair.value.toDouble());
                else if (pair.code == 41) insert.sx = pair.value.toDouble();
                else if (pair.code == 42) insert.sy = pair.value.toDouble();
                else if (pair.code == 50) insert.rotation = pair.value.toDouble();
                else if (pair.code == 1000 && pair.value.startsWith(QStringLiteral("FACE_ID=")))
                    insert.faceId = pair.value.mid(8);
                else if (pair.code == 1000 && pair.value.startsWith(QStringLiteral("DIR=")))
                    insert.direction = pair.value.mid(4);
            }
            if (!insert.faceId.isEmpty()) inserts->append(insert);
        }
        i = end;
    }
    return true;
}

bool quadCorners(const QVector<MapTriangle> &triangles, QVector<QVector3D> *corners) {
    QVector<QVector3D> vertices;
    for (const auto &triangle : triangles) for (const auto &point : {triangle.a, triangle.b, triangle.c}) {
        if (std::none_of(vertices.cbegin(), vertices.cend(), [&](const auto &v) { return (v-point).length() < 0.0001f; }))
            vertices.append(point);
    }
    if (vertices.size() != 4) return false;
    double minZ = std::numeric_limits<double>::infinity(), maxZ = -minZ;
    for (const auto &v : vertices) { minZ = std::min(minZ, double(v.z())); maxZ = std::max(maxZ, double(v.z())); }
    if (maxZ-minZ < 0.1) return false;
    QVector<QVector3D> bottom, top;
    for (const auto &v : vertices) (std::abs(v.z()-minZ) < std::abs(v.z()-maxZ) ? bottom : top).append(v);
    if (bottom.size() != 2 || top.size() != 2) return false;
    const auto &first = triangles.first();
    const int bottomFirst = (std::abs(first.a.z()-minZ) < std::abs(first.a.z()-maxZ)) +
                            (std::abs(first.b.z()-minZ) < std::abs(first.b.z()-maxZ)) +
                            (std::abs(first.c.z()-minZ) < std::abs(first.c.z()-maxZ));
    QVector3D bottomLeft, bottomRight, topLeft, topRight;
    if (bottomFirst == 2) {
        const auto topCandidate = std::abs(first.a.z()-maxZ) < std::abs(first.a.z()-minZ) ? first.a :
                                  std::abs(first.b.z()-maxZ) < std::abs(first.b.z()-minZ) ? first.b : first.c;
        topLeft = topCandidate;
        bottomLeft = (bottom[0]-topLeft).lengthSquared() < (bottom[1]-topLeft).lengthSquared() ? bottom[0] : bottom[1];
        bottomRight = (bottom[0]-bottomLeft).lengthSquared() < 1e-8 ? bottom[1] : bottom[0];
        topRight = (top[0]-topLeft).lengthSquared() < 1e-8 ? top[1] : top[0];
    } else if (bottomFirst == 1) {
        bottomLeft = std::abs(first.a.z()-minZ) < std::abs(first.a.z()-maxZ) ? first.a :
                     std::abs(first.b.z()-minZ) < std::abs(first.b.z()-maxZ) ? first.b : first.c;
        topLeft = (top[0]-bottomLeft).lengthSquared() < (top[1]-bottomLeft).lengthSquared() ? top[0] : top[1];
        topRight = (top[0]-topLeft).lengthSquared() < 1e-8 ? top[1] : top[0];
        bottomRight = (bottom[0]-bottomLeft).lengthSquared() < 1e-8 ? bottom[1] : bottom[0];
    } else return false;
    *corners = {topLeft, bottomLeft, bottomRight, topRight};
    return true;
}

bool populateTriangleMaps(const QVector<QVector3D> &corners, const QVector<QPointF> &destination,
                          const QVector<MapTriangle> &triangles, UnrollFace *face) {
    for (const auto &triangle : triangles) {
        QPointF mapped[3];
        const QVector3D source[3]{triangle.a, triangle.b, triangle.c};
        for (int i = 0; i < 3; ++i) {
            int corner = 0;
            for (int j = 1; j < 4; ++j)
                if ((source[i]-corners[j]).lengthSquared() < (source[i]-corners[corner]).lengthSquared()) corner = j;
            if ((source[i]-corners[corner]).length() > 0.001f) return false;
            mapped[i] = destination[corner];
        }
        face->triangles.append({triangle, mapped[0], mapped[1], mapped[2]});
    }
    return true;
}

bool makeUnrollFace(const UnrollInsert &insert, const QVector<QPointF> &block,
                    const QVector<MapTriangle> &triangles, UnrollFace *face) {
    if (std::abs(insert.rotation) > 1e-6 || insert.sx <= 0 || insert.sy <= 0) return false;
    QVector<QVector3D> corners;
    if (!quadCorners(triangles, &corners)) return false;
    QVector<QPointF> polygon;
    for (const auto &point : block)
        polygon.append(QPointF((insert.position.x()+point.x()*insert.sx)*0.001,
                                -(insert.position.y()+point.y()*insert.sy)*0.001));
    if (polygon.size() != 4) return false;
    double lowY = std::numeric_limits<double>::infinity(), highY = -lowY;
    for (const auto &point : polygon) { lowY = std::min(lowY, point.y()); highY = std::max(highY, point.y()); }
    QVector<QPointF> top, bottom;
    for (const auto &point : polygon) (std::abs(point.y()-lowY) < std::abs(point.y()-highY) ? top : bottom).append(point);
    if (top.size() != 2 || bottom.size() != 2) return false;
    const QVector<QPointF> destination{
        top[0].x() < top[1].x() ? top[0] : top[1],
        bottom[0].x() < bottom[1].x() ? bottom[0] : bottom[1],
        bottom[0].x() < bottom[1].x() ? bottom[1] : bottom[0],
        top[0].x() < top[1].x() ? top[1] : top[0]};
    face->faceId = insert.faceId;
    face->partId = insert.faceId.section(QLatin1Char('-'), 0, 3);
    face->direction = insert.direction;
    face->outline = destination;
    return populateTriangleMaps(corners, destination, triangles, face);
}

UnrollSheet generatedUnrollSheet(const QString &part, const QString &side,
                                 const QHash<QString, QVector<MapTriangle>> &faceTriangles) {
    struct Segment { QString id; QVector<QVector3D> corners; QVector<MapTriangle> triangles; int a = -1, b = -1; };
    QVector<Segment> segments;
    QVector<QPointF> nodes;
    auto nodeFor = [&nodes](const QVector3D &point) -> int {
        const QPointF xy(point.x(), point.y());
        for (int i = 0; i < nodes.size(); ++i)
            if (QLineF(xy, nodes[i]).length() < 0.005) return i;
        nodes.append(xy);
        return nodes.size()-1;
    };
    for (auto it = faceTriangles.cbegin(); it != faceTriangles.cend(); ++it) {
        if (it.value().isEmpty() || it.value().first().partId != part) continue;
        const QString faceSide = it.key().contains(QStringLiteral("-EX-")) ? QStringLiteral("EX") :
                                 it.key().contains(QStringLiteral("-IN-")) ? QStringLiteral("IN") : QStringLiteral("GEN");
        if (faceSide != side || it.key().contains(QStringLiteral("-CAP-"))) continue;
        QVector<QVector3D> corners;
        if (!quadCorners(it.value(), &corners)) {
            float minZ = std::numeric_limits<float>::max(), maxZ = -minZ;
            for (const auto &triangle : it.value()) for (const auto &point : {triangle.a, triangle.b, triangle.c}) {
                minZ = std::min(minZ, point.z()); maxZ = std::max(maxZ, point.z());
            }
            if (maxZ-minZ < 0.005f) continue;
            return {};
        }
        const auto sameXY = [](const QVector3D &a, const QVector3D &b) {
            return QLineF(QPointF(a.x(),a.y()), QPointF(b.x(),b.y())).length() < 0.005;
        };
        if (!sameXY(corners[0], corners[1]) || !sameXY(corners[2], corners[3]) ||
            std::abs(corners[0].z()-corners[3].z()) > 0.005 ||
            std::abs(corners[1].z()-corners[2].z()) > 0.005) return {};
        Segment segment;
        segment.id = it.key(); segment.corners = corners; segment.triangles = it.value();
        segment.a = nodeFor(corners[1]); segment.b = nodeFor(corners[2]);
        if (segment.a == segment.b) return {};
        segments.append(std::move(segment));
    }
    if (segments.isEmpty()) return {};
    QVector<int> degree(nodes.size(), 0);
    for (const auto &segment : segments) { ++degree[segment.a]; ++degree[segment.b]; }
    if (std::any_of(degree.cbegin(), degree.cend(), [](int value) { return value > 2; })) return {};
    QVector<int> ends;
    for (int i = 0; i < degree.size(); ++i) if (degree[i] == 1) ends.append(i);
    if (ends.size() != 0 && ends.size() != 2) return {};
    int current = ends.isEmpty() ? 0 : ends.first();
    const auto smaller = [&nodes](int a, int b) {
        return nodes[a].x() == nodes[b].x() ? nodes[a].y() < nodes[b].y() : nodes[a].x() < nodes[b].x();
    };
    if (ends.size() == 2 && smaller(ends[1], current)) current = ends[1];
    if (ends.isEmpty()) for (int i = 1; i < nodes.size(); ++i)
        if (smaller(i, current)) current = i;
    QVector<bool> used(segments.size(), false);
    UnrollSheet sheet;
    sheet.id = part + QLatin1Char('|') + side;
    sheet.series = part;
    sheet.side = side;
    sheet.parts.append(part);
    double offset = 0;
    for (int count = 0; count < segments.size(); ++count) {
        int chosen = -1;
        for (int i = 0; i < segments.size(); ++i) {
            if (used[i] || (segments[i].a != current && segments[i].b != current)) continue;
            if (chosen < 0 || segments[i].id < segments[chosen].id) chosen = i;
        }
        if (chosen < 0) return {};
        auto &segment = segments[chosen];
        const bool forward = segment.a == current;
        const double width = QLineF(nodes[segment.a], nodes[segment.b]).length();
        const QVector<QPointF> destination{
            QPointF(offset, -segment.corners[0].z()),
            QPointF(forward ? offset : offset+width, -segment.corners[1].z()),
            QPointF(forward ? offset+width : offset, -segment.corners[2].z()),
            QPointF(offset+width, -segment.corners[3].z())};
        QVector<QPointF> mapped = destination;
        if (!forward) {
            mapped[0].setX(offset+width);
            mapped[3].setX(offset);
        }
        UnrollFace face;
        face.faceId = segment.id; face.partId = part;
        face.direction = segment.id.section(QLatin1Char('-'), -2, -2);
        face.outline = mapped;
        if (!populateTriangleMaps(segment.corners, mapped, segment.triangles, &face)) return {};
        sheet.faces.append(std::move(face));
        used[chosen] = true;
        current = forward ? segment.b : segment.a;
        offset += width;
    }
    return sheet;
}

double coordinate(const QVector3D &point, int axis) {
    return axis == 0 ? point.x() : axis == 1 ? point.y() : point.z();
}

bool modelMatrix(const QString &path, QMatrix4x4 *matrix, QString *error) {
    matrix->setToIdentity();
    if (!QFileInfo::exists(path)) {
        if (error) *error = QStringLiteral("Model metadata is required: ") + path;
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    QXmlStreamReader xml(&file);
    while (xml.readNextStartElement()) {
        if (xml.name() != QStringLiteral("Transform")) continue;
        const auto attrs = xml.attributes();
        auto number = [&attrs](const char *name, double fallback) {
            const auto value = attrs.value(QLatin1StringView(name));
            return value.isEmpty() ? fallback : value.toDouble();
        };
        const double sx = number("sx", 1), sy = number("sy", 1), sz = number("sz", 1);
        if (!(sx > 0 && sy > 0 && sz > 0)) break;
        matrix->translate(number("tx", 0), number("ty", 0), number("tz", 0));
        matrix->rotate(number("rz", 0), 0, 0, 1);
        matrix->rotate(number("ry", 0), 0, 1, 0);
        matrix->rotate(number("rx", 0), 1, 0, 0);
        matrix->scale(sx, sy, sz);
        return true;
    }
    if (error) *error = QStringLiteral("Invalid model metadata: ") + path;
    return false;
}

bool readObj(const QString &path, const QMatrix4x4 &matrix, QVector<MapTriangle> *triangles,
             QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = file.errorString();
        return false;
    }
    const QString partId = QFileInfo(path).baseName();
    QVector<QVector3D> vertices;
    QString faceId;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        const QStringList tokens = line.simplified().split(QLatin1Char(' '));
        if (tokens[0] == QStringLiteral("v") && tokens.size() >= 4) {
            bool xOk = false, yOk = false, zOk = false;
            const float x = tokens[1].toFloat(&xOk), y = tokens[2].toFloat(&yOk);
            const float z = tokens[3].toFloat(&zOk);
            if (!xOk || !yOk || !zOk) {
                if (error) *error = QStringLiteral("Invalid OBJ vertex: ") + path;
                return false;
            }
            vertices.append(matrix.map(QVector3D(x, y, z)));
        } else if (tokens[0] == QStringLiteral("g")) {
            faceId = tokens.size() > 1 ? tokens[1] : QString();
        } else if (tokens[0] == QStringLiteral("f") && tokens.size() >= 4) {
            if (faceId.isEmpty()) continue;
            QVector<int> indices;
            for (const auto &token : tokens.mid(1)) {
                if (token.startsWith(QLatin1Char('#'))) break;
                bool valid = false;
                int index = token.section(QLatin1Char('/'), 0, 0).toInt(&valid);
                index = index < 0 ? vertices.size() + index : index - 1;
                if (!valid || index < 0 || index >= vertices.size()) {
                    if (error) *error = QStringLiteral("Invalid OBJ face index: ") + path;
                    return false;
                }
                indices.append(index);
            }
            for (int i = 1; i + 1 < indices.size(); ++i)
                triangles->append({vertices[indices[0]], vertices[indices[i]], vertices[indices[i + 1]],
                                   partId, faceId});
        }
    }
    return true;
}

bool readDxf(const QString &path, const QMatrix4x4 &matrix, MapDrawing *drawing, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = file.errorString();
        return false;
    }
    QVector<DxfPair> pairs;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString codeLine = stream.readLine().trimmed();
        if (stream.atEnd()) break;
        bool valid = false;
        const int code = codeLine.toInt(&valid);
        const QString value = stream.readLine().trimmed();
        if (!valid) {
            if (error) *error = QStringLiteral("Invalid DXF group code: ") + path;
            return false;
        }
        pairs.append({code, value});
    }
    drawing->id = QFileInfo(path).baseName();
    drawing->partId = QFileInfo(path).dir().dirName();
    drawing->path = path;
    bool entities = false;
    QVector<QVector3D> line;
    QString faceId;
    auto finishLine = [&] {
        if (line.size() > 1) drawing->lines.append(line);
        line.clear();
        faceId.clear();
    };
    for (int i = 0; i < pairs.size(); ++i) {
        if (pairs[i].code == 0 && pairs[i].value == QStringLiteral("SECTION") &&
            i + 1 < pairs.size() && pairs[i + 1].code == 2) {
            entities = pairs[i + 1].value == QStringLiteral("ENTITIES");
            continue;
        }
        if (!entities) continue;
        if (pairs[i].code == 0 && pairs[i].value == QStringLiteral("ENDSEC")) {
            finishLine(); entities = false; continue;
        }
        if (pairs[i].code != 0) continue;
        const QString type = pairs[i].value;
        if (type == QStringLiteral("POLYLINE")) {
            finishLine();
            for (int j = i + 1; j < pairs.size() && pairs[j].code != 0; ++j)
                if (pairs[j].code == 1000 && pairs[j].value.startsWith(QStringLiteral("FACE_ID="))) {
                    faceId = pairs[j].value.mid(8);
                    for (const auto &id : faceId.split(QLatin1Char(','), Qt::SkipEmptyParts))
                        drawing->faceIds.insert(id.trimmed());
                }
        } else if (type == QStringLiteral("VERTEX")) {
            double xyz[3]{0, 0, 0};
            for (int j = i + 1; j < pairs.size() && pairs[j].code != 0; ++j)
                if (pairs[j].code == 10 || pairs[j].code == 20 || pairs[j].code == 30)
                    xyz[pairs[j].code / 10 - 1] = pairs[j].value.toDouble();
            line.append(matrix.map(QVector3D(float(xyz[0]), float(xyz[1]), float(xyz[2]))));
        } else if (type == QStringLiteral("SEQEND")) finishLine();
    }
    finishLine();
    if (drawing->lines.isEmpty()) {
        if (error) *error = QStringLiteral("No supported drawing polylines: ") + path;
        return false;
    }
    double low[3]{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
                  std::numeric_limits<double>::infinity()};
    double high[3]{-low[0], -low[1], -low[2]};
    for (const auto &pathLine : drawing->lines)
        for (const auto &point : pathLine)
            for (int axis = 0; axis < 3; ++axis) {
                low[axis] = std::min(low[axis], coordinate(point, axis));
                high[axis] = std::max(high[axis], coordinate(point, axis));
            }
    const double spans[3]{high[0] - low[0], high[1] - low[1], high[2] - low[2]};
    const int flat = std::min_element(std::begin(spans), std::end(spans)) - std::begin(spans);
    if (flat == 2) { drawing->horizontalAxis = 0; drawing->verticalAxis = 1; }
    else { drawing->horizontalAxis = flat == 0 ? 1 : 0; drawing->verticalAxis = 2; }
    return true;
}
}

bool InspectionMapEngine::load(const QString &root, QString *error) {
    triangles_.clear(); drawings_.clear(); faceDrawing_.clear(); unrollSheets_.clear();
    const QDir source(root);
    const QString modelRoot = source.exists(QStringLiteral("model")) ? source.filePath(QStringLiteral("model")) : root;
    QDirIterator objects(modelRoot, {QStringLiteral("*.obj")}, QDir::Files, QDirIterator::Subdirectories);
    QHash<QString, QMatrix4x4> matrices;
    while (objects.hasNext()) {
        const QString path = objects.next();
        QMatrix4x4 matrix;
        if (!modelMatrix(QFileInfo(path).dir().filePath(QStringLiteral("meta.gxxml")), &matrix, error) ||
            !readObj(path, matrix, &triangles_, error)) return false;
        matrices.insert(QFileInfo(path).baseName(), matrix);
    }
    if (triangles_.isEmpty()) {
        if (error) *error = QStringLiteral("No model faces found: ") + modelRoot;
        return false;
    }
    const QString drawingRoot = source.exists(QStringLiteral("map")) ? source.filePath(QStringLiteral("map")) :
        QDir(modelRoot).filePath(QStringLiteral("../map"));
    QDirIterator files(drawingRoot, {QStringLiteral("*.dxf")}, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
        const QString partId = QFileInfo(path).dir().dirName();
        if (!matrices.contains(partId)) continue;
        MapDrawing drawing;
        if (!readDxf(path, matrices.value(partId), &drawing, error)) return false;
        drawings_.append(std::move(drawing));
    }
    std::sort(drawings_.begin(), drawings_.end(), [](const auto &a, const auto &b) {
        return a.id < b.id;
    });
    QSet<QString> explicitFaces;
    for (const auto &drawing : drawings_) explicitFaces.unite(drawing.faceIds);
    QHash<QString, QSet<QString>> modelFaces;
    for (const auto &triangle : triangles_) modelFaces[triangle.partId].insert(triangle.faceId);
    QHash<QString, int> inferredOwners;
    for (int i = 0; i < drawings_.size(); ++i) {
        const auto &drawing = drawings_[i];
        if (!drawing.faceIds.isEmpty() ||
            !drawing.id.startsWith(drawing.partId + QLatin1Char('-'))) continue;
        QString prefix = drawing.id + QLatin1Char('-');
        if (drawing.id == drawing.partId + QStringLiteral("-TP") ||
            drawing.id == drawing.partId + QStringLiteral("-BT"))
            prefix = drawing.partId + QStringLiteral("-CAP-") + drawing.id.section(QLatin1Char('-'), -1) + QLatin1Char('-');
        for (const auto &faceId : modelFaces.value(drawing.partId)) {
            if (explicitFaces.contains(faceId) || !faceId.startsWith(prefix)) continue;
            const auto owner = inferredOwners.constFind(faceId);
            if (owner == inferredOwners.cend()) inferredOwners.insert(faceId, i);
            else if (owner.value() != i) inferredOwners[faceId] = -1;
        }
    }
    for (auto it = inferredOwners.cbegin(); it != inferredOwners.cend(); ++it)
        if (it.value() >= 0) drawings_[it.value()].faceIds.insert(it.key());
    faceDrawing_.clear();
    for (int i = 0; i < drawings_.size(); ++i)
        for (const auto &faceId : drawings_[i].faceIds)
            if (!faceDrawing_.contains(faceId)) faceDrawing_.insert(faceId, i);
    QHash<QString, QVector<MapTriangle>> faceTriangles;
    for (const auto &triangle : triangles_) faceTriangles[triangle.faceId].append(triangle);
    const QDir unrollRoot(source.filePath(QStringLiteral("unroll")));
    if (unrollRoot.exists()) {
        QDirIterator files(unrollRoot.path(), {QStringLiteral("*.dxf")}, QDir::Files);
        QHash<QString, int> sheetIndex;
        while (files.hasNext()) {
            QHash<QString, QVector<QPointF>> blocks;
            QVector<UnrollInsert> inserts;
            QString ignored;
            if (!readUnrollDxf(files.next(), &blocks, &inserts, &ignored)) continue;
            for (const auto &insert : inserts) {
                if (!blocks.contains(insert.block) || !faceTriangles.contains(insert.faceId)) continue;
                UnrollFace face;
                if (!makeUnrollFace(insert, blocks.value(insert.block), faceTriangles.value(insert.faceId), &face)) continue;
                const QString side = insert.faceId.section(QLatin1Char('-'), 4, 4);
                if (side != QStringLiteral("EX") && side != QStringLiteral("IN")) continue;
                const QString series = face.partId.section(QLatin1Char('-'), 0, 2);
                if (!face.partId.mid(series.size()).startsWith(QStringLiteral("-R"))) continue;
                const QString id = series + QLatin1Char('|') + side;
                if (!sheetIndex.contains(id)) {
                    sheetIndex.insert(id, unrollSheets_.size());
                    UnrollSheet sheet; sheet.id = id; sheet.series = series; sheet.side = side; sheet.sourceDxf = true;
                    unrollSheets_.append(sheet);
                }
                auto &sheet = unrollSheets_[sheetIndex.value(id)];
                if (!sheet.parts.contains(face.partId)) sheet.parts.append(face.partId);
                sheet.faces.append(std::move(face));
            }
        }
        const QStringList directions{QStringLiteral("FR"), QStringLiteral("RG"),
                                     QStringLiteral("RR"), QStringLiteral("LF")};
        for (auto &sheet : unrollSheets_) {
            sheet.parts.sort();
            double offset = 0;
            for (const auto &direction : directions) {
                double low = std::numeric_limits<double>::infinity(), high = -low;
                for (const auto &face : sheet.faces) if (face.direction == direction)
                    for (const auto &point : face.outline) {
                        low = std::min(low, point.x()); high = std::max(high, point.x());
                    }
                if (!(high > low)) continue;
                const double shift = offset - low;
                for (auto &face : sheet.faces) if (face.direction == direction) {
                    for (auto &point : face.outline) point.rx() += shift;
                    for (auto &triangle : face.triangles) {
                        triangle.a.rx() += shift; triangle.b.rx() += shift; triangle.c.rx() += shift;
                    }
                }
                offset += high-low;
            }
        }
        std::sort(unrollSheets_.begin(), unrollSheets_.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    }
    const QStringList sides{QStringLiteral("EX"), QStringLiteral("IN"), QStringLiteral("GEN")};
    for (const auto &part : parts()) for (const auto &side : sides) {
        const bool covered = std::any_of(unrollSheets_.cbegin(), unrollSheets_.cend(), [&](const auto &sheet) {
            return sheet.side == side && sheet.parts.contains(part);
        });
        if (covered) continue;
        auto generated = generatedUnrollSheet(part, side, faceTriangles);
        if (!generated.faces.isEmpty()) unrollSheets_.append(std::move(generated));
    }
    std::sort(unrollSheets_.begin(), unrollSheets_.end(), [](const auto &a, const auto &b) { return a.id < b.id; });
    return true;
}

MapHit InspectionMapEngine::cast(const QVector3D &origin, const QVector3D &direction) const {
    MapHit result;
    double nearest = std::numeric_limits<double>::infinity();
    for (const auto &triangle : triangles_) {
        const QVector3D edge1 = triangle.b - triangle.a, edge2 = triangle.c - triangle.a;
        const QVector3D cross = QVector3D::crossProduct(direction, edge2);
        const double determinant = QVector3D::dotProduct(edge1, cross);
        if (std::abs(determinant) < 1e-9) continue;
        const QVector3D offset = origin - triangle.a;
        const double u = QVector3D::dotProduct(offset, cross) / determinant;
        if (u < -1e-5 || u > 1.00001) continue;
        const QVector3D q = QVector3D::crossProduct(offset, edge1);
        const double v = QVector3D::dotProduct(direction, q) / determinant;
        if (v < -1e-5 || u + v > 1.00001) continue;
        const double t = QVector3D::dotProduct(edge2, q) / determinant;
        if (t <= 1e-4 || t >= nearest) continue;
        nearest = t;
        result = {true, origin + direction * float(t), triangle.partId, triangle.faceId, t};
    }
    return result;
}

QStringList InspectionMapEngine::parts() const {
    QSet<QString> names;
    for (const auto &triangle : triangles_) names.insert(triangle.partId);
    QStringList result = names.values();
    result.sort();
    return result;
}

const MapDrawing *InspectionMapEngine::drawing(const QString &id) const {
    for (const auto &drawing : drawings_) if (drawing.id == id) return &drawing;
    return nullptr;
}

const MapDrawing *InspectionMapEngine::drawingForFace(const QString &faceId) const {
    const auto found = faceDrawing_.constFind(faceId);
    return found == faceDrawing_.cend() ? nullptr : &drawings_[found.value()];
}

bool InspectionMapEngine::mapPoint(const QString &drawingId, const QString &faceId,
                                   const QPointF &point, QVector3D *world) const {
    const auto *target = drawing(drawingId);
    if (!target || !target->faceIds.contains(faceId)) return false;
    for (const auto &triangle : triangles_) {
        if (triangle.faceId != faceId) continue;
        const QPointF a = drawingPoint(*target, triangle.a), b = drawingPoint(*target, triangle.b);
        const QPointF c = drawingPoint(*target, triangle.c);
        const double denominator = (b.y()-c.y())*(a.x()-c.x()) + (c.x()-b.x())*(a.y()-c.y());
        if (std::abs(denominator) < 1e-10) continue;
        const double u = ((b.y()-c.y())*(point.x()-c.x()) + (c.x()-b.x())*(point.y()-c.y())) / denominator;
        const double v = ((c.y()-a.y())*(point.x()-c.x()) + (a.x()-c.x())*(point.y()-c.y())) / denominator;
        const double w = 1-u-v;
        if (u < -0.0001 || v < -0.0001 || w < -0.0001) continue;
        if (world) *world = triangle.a*float(u) + triangle.b*float(v) + triangle.c*float(w);
        return true;
    }
    return false;
}

const UnrollSheet *InspectionMapEngine::unrollSheet(const QString &id) const {
    for (const auto &sheet : unrollSheets_) if (sheet.id == id) return &sheet;
    return nullptr;
}

bool UnrollSheet::point(const QString &faceId, const QVector3D &world, QPointF *unrolled) const {
    for (const auto &face : faces) if (face.faceId == faceId) {
        for (const auto &triangle : face.triangles) {
            const QVector3D u = triangle.source.b - triangle.source.a;
            const QVector3D v = triangle.source.c - triangle.source.a;
            const QVector3D w = world - triangle.source.a;
            const double uu = QVector3D::dotProduct(u, u), uv = QVector3D::dotProduct(u, v);
            const double vv = QVector3D::dotProduct(v, v), wu = QVector3D::dotProduct(w, u);
            const double wv = QVector3D::dotProduct(w, v), determinant = uu*vv-uv*uv;
            if (std::abs(determinant) < 1e-12) continue;
            const double b = (wu*vv-wv*uv)/determinant, c = (wv*uu-wu*uv)/determinant;
            const double a = 1-b-c;
            if (a < -0.001 || b < -0.001 || c < -0.001) continue;
            const QVector3D projected = triangle.source.a*float(a) + triangle.source.b*float(b) +
                                        triangle.source.c*float(c);
            if ((projected-world).length() > 0.01f) continue;
            if (unrolled) *unrolled = triangle.a*a + triangle.b*b + triangle.c*c;
            return true;
        }
    }
    return false;
}

bool UnrollChart::point(const QString &faceId, const QVector3D &world, QPointF *unrolled) const {
    for (const auto &strip : strips) {
        if (strip.faceId != faceId) continue;
        const QVector3D direction = (strip.end - strip.start).normalized();
        const double distance = QVector3D::dotProduct(world - strip.start, direction);
        if (distance < -0.05 || distance > strip.width + 0.05) return false;
        if (unrolled) *unrolled = QPointF(strip.offset + std::clamp(distance, 0.0, strip.width), -world.z());
        return true;
    }
    return false;
}

UnrollChart InspectionMapEngine::unroll(const QString &partId) const {
    UnrollChart chart;
    chart.partId = partId;
    QHash<QString, QVector<QVector3D>> faces;
    for (const auto &triangle : triangles_) {
        if (triangle.partId != partId) continue;
        auto &vertices = faces[triangle.faceId];
        vertices.append(triangle.a); vertices.append(triangle.b); vertices.append(triangle.c);
    }
    QVector3D center;
    int centerCount = 0;
    for (const auto &vertices : faces) for (const auto &v : vertices) { center += v; ++centerCount; }
    if (centerCount) center /= float(centerCount);
    for (auto it = faces.cbegin(); it != faces.cend(); ++it) {
        const auto &vertices = it.value();
        if (vertices.size() < 3) continue;
        double mx = 0, my = 0, xx = 0, xy = 0, yy = 0;
        double bottom = std::numeric_limits<double>::infinity(), top = -bottom;
        for (const auto &v : vertices) { mx += v.x(); my += v.y(); bottom = std::min(bottom, double(v.z())); top = std::max(top, double(v.z())); }
        mx /= vertices.size(); my /= vertices.size();
        for (const auto &v : vertices) { const double x = v.x() - mx, y = v.y() - my; xx += x*x; xy += x*y; yy += y*y; }
        const double angle = 0.5 * std::atan2(2*xy, xx - yy);
        QVector3D tangent(float(std::cos(angle)), float(std::sin(angle)), 0);
        double low = std::numeric_limits<double>::infinity(), high = -low, spread = 0;
        for (const auto &v : vertices) {
            const double along = (v.x() - mx)*tangent.x() + (v.y() - my)*tangent.y();
            const double across = -(v.x() - mx)*tangent.y() + (v.y() - my)*tangent.x();
            low = std::min(low, along); high = std::max(high, along);
            spread = std::max(spread, std::abs(across));
        }
        const double width = high - low;
        if (top - bottom < 0.1 || width < 0.1 || spread > std::max(0.05, width * 0.08)) {
            chart.excludedFaces.append(it.key());
            continue;
        }
        const QVector3D midpoint(float(mx), float(my), 0);
        const QVector3D radial = midpoint - QVector3D(center.x(), center.y(), 0);
        if (tangent.x()*radial.y() - tangent.y()*radial.x() < 0) tangent = -tangent;
        const QVector3D start = midpoint - tangent * float(width / 2);
        chart.strips.append({it.key(), start, start + tangent * float(width), 0, width,
                             bottom, top, it.key().contains(QStringLiteral("-IN-"))});
    }
    std::sort(chart.strips.begin(), chart.strips.end(), [&center](const auto &a, const auto &b) {
        if (a.interior != b.interior) return !a.interior;
        const auto am = (a.start + a.end) / 2 - center, bm = (b.start + b.end) / 2 - center;
        const double aa = std::atan2(double(am.y()), double(am.x()));
        const double ba = std::atan2(double(bm.y()), double(bm.x()));
        return aa == ba ? a.faceId < b.faceId : aa < ba;
    });
    for (auto &strip : chart.strips) {
        double &offset = strip.interior ? chart.interiorWidth : chart.exteriorWidth;
        strip.offset = offset;
        offset += strip.width;
    }
    for (auto &strip : chart.strips) if (strip.interior) strip.offset += chart.exteriorWidth + 0.5;
    chart.excludedFaces.sort();
    return chart;
}

QPointF InspectionMapEngine::drawingPoint(const MapDrawing &drawing, const QVector3D &point) {
    return {coordinate(point, drawing.horizontalAxis), -coordinate(point, drawing.verticalAxis)};
}

QVector3D InspectionMapEngine::cameraRay(const QVector3D &pixel, double fx, double fy, double cx, double cy,
                                         const QQuaternion &orientation) {
    const QVector3D local(1, float((pixel.x() - cx) / fx), float(-(pixel.y() - cy) / fy));
    return orientation.rotatedVector(local).normalized();
}
