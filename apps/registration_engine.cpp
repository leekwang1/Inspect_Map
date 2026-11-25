#include "registration_engine.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMatrix3x3>
#include <QMatrix4x4>
#include <QTextStream>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>
#include <vtkAppendPolyData.h>
#include <vtkCellArray.h>
#include <vtkGenericCell.h>
#include <vtkIterativeClosestPointTransform.h>
#include <vtkLandmarkTransform.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkOBJReader.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkStaticCellLocator.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>

namespace {
bool modelMatrix(const QString &path, double fallbackScale, QMatrix4x4 *matrix, QString *error) {
    double tx = 0, ty = 0, tz = 0, rx = 0, ry = 0, rz = 0;
    double sx = fallbackScale, sy = fallbackScale, sz = fallbackScale;
    if (QFileInfo::exists(path)) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = file.errorString();
            return false;
        }
        QXmlStreamReader xml(&file);
        bool found = false;
        while (xml.readNextStartElement()) {
            if (xml.name() == QStringLiteral("Transform")) {
                const auto attributes = xml.attributes();
                auto number = [&attributes](const char *name, double defaultValue) {
                    const auto value = attributes.value(QLatin1StringView(name));
                    return value.isEmpty() ? defaultValue : value.toDouble();
                };
                tx = number("tx", 0); ty = number("ty", 0); tz = number("tz", 0);
                rx = number("rx", 0); ry = number("ry", 0); rz = number("rz", 0);
                sx = number("sx", fallbackScale); sy = number("sy", fallbackScale);
                sz = number("sz", fallbackScale);
                found = true;
                break;
            }
        }
        if (!found || xml.hasError()) {
            if (error) *error = QStringLiteral("Invalid model metadata: ") + path;
            return false;
        }
    }
    if (!(sx > 0 && sy > 0 && sz > 0)) {
        if (error) *error = QStringLiteral("Model scale must be positive: ") + path;
        return false;
    }
    matrix->setToIdentity();
    matrix->translate(tx, ty, tz);
    matrix->rotate(rz, 0, 0, 1);
    matrix->rotate(ry, 0, 1, 0);
    matrix->rotate(rx, 1, 0, 0);
    matrix->scale(sx, sy, sz);
    return true;
}

bool nonCollinear(const QVector<QVector3D> &points) {
    for (int a = 0; a < points.size(); ++a)
        for (int b = a + 1; b < points.size(); ++b)
            for (int c = b + 1; c < points.size(); ++c) {
                const auto u = points[b] - points[a];
                const auto v = points[c] - points[a];
                if (QVector3D::crossProduct(u, v).lengthSquared() >
                    1e-8f * u.lengthSquared() * v.lengthSquared()) return true;
            }
    return false;
}

bool classifiedFaces(const QString &path, const QMatrix4x4 &coordinate,
                     vtkSmartPointer<vtkPolyData> *exterior,
                     vtkSmartPointer<vtkPolyData> *interior, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = file.errorString();
        return false;
    }
    struct FaceBuilder {
        vtkSmartPointer<vtkPoints> points = vtkSmartPointer<vtkPoints>::New();
        vtkSmartPointer<vtkCellArray> faces = vtkSmartPointer<vtkCellArray>::New();
        QHash<int, vtkIdType> indices;
    } outer, inner;
    QVector<QVector3D> vertices;
    FaceBuilder *active = nullptr;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        const QStringList fields = line.simplified().split(QLatin1Char(' '));
        if (fields[0] == QStringLiteral("v") && fields.size() >= 4) {
            bool xOk = false, yOk = false, zOk = false;
            const float x = fields[1].toFloat(&xOk);
            const float y = fields[2].toFloat(&yOk);
            const float z = fields[3].toFloat(&zOk);
            if (!xOk || !yOk || !zOk) {
                if (error) *error = QStringLiteral("Invalid OBJ vertex: ") + path;
                return false;
            }
            vertices.append(coordinate.map(QVector3D(x, y, z)));
        } else if (fields[0] == QStringLiteral("g")) {
            active = nullptr;
            for (const auto &name : fields.mid(1)) {
                if (name.contains(QStringLiteral("-EX-"), Qt::CaseInsensitive)) active = &outer;
                if (name.contains(QStringLiteral("-IN-"), Qt::CaseInsensitive)) active = &inner;
            }
        } else if (fields[0] == QStringLiteral("f") && active) {
            if (fields.size() < 4) {
                if (error) *error = QStringLiteral("Invalid OBJ face: ") + path;
                return false;
            }
            QVector<int> vertexIds;
            for (const auto &token : fields.mid(1)) {
                if (token.startsWith(QLatin1Char('#'))) break;
                bool parsed = false;
                int index = token.section(QLatin1Char('/'), 0, 0).toInt(&parsed);
                if (index < 0) index = vertices.size() + index;
                else --index;
                if (!parsed || index < 0 || index >= vertices.size()) {
                    if (error) *error = QStringLiteral("Invalid OBJ face index: ") + path;
                    return false;
                }
                vertexIds.append(index);
            }
            if (vertexIds.size() < 3) {
                if (error) *error = QStringLiteral("Invalid OBJ face: ") + path;
                return false;
            }
            active->faces->InsertNextCell(vertexIds.size());
            for (const int index : vertexIds) {
                auto found = active->indices.constFind(index);
                vtkIdType pointId;
                if (found == active->indices.cend()) {
                    const auto &point = vertices[index];
                    pointId = active->points->InsertNextPoint(point.x(), point.y(), point.z());
                    active->indices.insert(index, pointId);
                } else pointId = found.value();
                active->faces->InsertCellPoint(pointId);
            }
        }
    }
    auto build = [](const FaceBuilder &builder) {
        if (builder.faces->GetNumberOfCells() == 0) return vtkSmartPointer<vtkPolyData>{};
        auto mesh = vtkSmartPointer<vtkPolyData>::New();
        mesh->SetPoints(builder.points);
        mesh->SetPolys(builder.faces);
        return mesh;
    };
    *exterior = build(outer);
    *interior = build(inner);
    return true;
}
}

bool RegistrationEngine::loadModel(const QString &root, double fallbackScale,
                                   QVector<RegistrationPart> *parts, vtkPolyData *combined, QString *error) {
    if (!parts || !combined || !QDir(root).exists()) {
        if (error) *error = QStringLiteral("Model directory not found.");
        return false;
    }
    const QString modelRoot = QDir(root).exists(QStringLiteral("model")) ?
        QDir(root).filePath(QStringLiteral("model")) : root;
    QVector<RegistrationPart> loaded;
    vtkNew<vtkAppendPolyData> append;
    QDirIterator files(modelRoot, {QStringLiteral("*.obj")}, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
        vtkNew<vtkOBJReader> reader;
        reader->SetFileName(QFile::encodeName(path).constData());
        reader->Update();
        if (!reader->GetOutput() || reader->GetOutput()->GetNumberOfPolys() == 0) {
            if (error) *error = QStringLiteral("OBJ has no faces: ") + path;
            return false;
        }
        QMatrix4x4 coordinate;
        const QString meta = QFileInfo(path).dir().filePath(QStringLiteral("meta.gxxml"));
        if (!modelMatrix(meta, fallbackScale, &coordinate, error)) return false;
        vtkNew<vtkPolyData> geometry;
        geometry->DeepCopy(reader->GetOutput());
        auto *vertices = geometry->GetPoints();
        for (vtkIdType i = 0; i < vertices->GetNumberOfPoints(); ++i) {
            double p[3]; vertices->GetPoint(i, p);
            const auto q = coordinate.map(QVector3D(float(p[0]), float(p[1]), float(p[2])));
            vertices->SetPoint(i, q.x(), q.y(), q.z());
        }
        vertices->Modified();
        vtkSmartPointer<vtkPolyData> exterior, interior;
        if (!classifiedFaces(path, coordinate, &exterior, &interior, error)) return false;
        append->AddInputData(geometry);
        loaded.append({QFileInfo(path).baseName(), geometry, exterior, interior});
    }
    if (loaded.isEmpty()) {
        if (error) *error = QStringLiteral("No OBJ files found in model directory.");
        return false;
    }
    append->Update();
    combined->DeepCopy(append->GetOutput());
    *parts = std::move(loaded);
    return true;
}

vtkSmartPointer<vtkPolyData> RegistrationEngine::cloudGeometry(const QVector<QVector3D> &points) {
    vtkNew<vtkPoints> vertices;
    vtkNew<vtkCellArray> cells;
    for (const auto &point : points) {
        const vtkIdType id = vertices->InsertNextPoint(point.x(), point.y(), point.z());
        cells->InsertNextCell(1);
        cells->InsertCellPoint(id);
    }
    auto geometry = vtkSmartPointer<vtkPolyData>::New();
    geometry->SetPoints(vertices);
    geometry->SetVerts(cells);
    return geometry;
}

bool RegistrationEngine::landmarks(const QVector<QVector3D> &source,
                                   const QVector<QVector3D> &target, bool allowScale,
                                   vtkMatrix4x4 *matrix, double *rmse, QString *error) {
    if (!matrix || source.size() < 3 || source.size() != target.size() ||
        !nonCollinear(source) || !nonCollinear(target)) {
        if (error) *error = QStringLiteral("At least three non-collinear point pairs are required.");
        return false;
    }
    vtkNew<vtkPoints> from;
    vtkNew<vtkPoints> to;
    for (int i = 0; i < source.size(); ++i) {
        from->InsertNextPoint(source[i].x(), source[i].y(), source[i].z());
        to->InsertNextPoint(target[i].x(), target[i].y(), target[i].z());
    }
    vtkNew<vtkLandmarkTransform> fit;
    fit->SetSourceLandmarks(from);
    fit->SetTargetLandmarks(to);
    if (allowScale) fit->SetModeToSimilarity();
    else fit->SetModeToRigidBody();
    fit->Update();
    matrix->DeepCopy(fit->GetMatrix());
    double sum = 0;
    for (int i = 0; i < source.size(); ++i) {
        const auto difference = transformPoint(matrix, source[i]) - target[i];
        sum += double(difference.lengthSquared());
    }
    if (rmse) *rmse = std::sqrt(sum / source.size());
    return true;
}

bool RegistrationEngine::icp(vtkPolyData *source, vtkPolyData *target,
                             vtkMatrix4x4 *initial, bool allowScale, vtkMatrix4x4 *matrix,
                             int *iterations, QString *error) {
    if (!source || !target || !matrix || source->GetNumberOfPoints() < 3 ||
        target->GetNumberOfCells() < 3) {
        if (error) *error = QStringLiteral("Point cloud and model are required for ICP.");
        return false;
    }
    vtkNew<vtkTransform> start;
    if (initial) start->SetMatrix(initial);
    vtkNew<vtkTransformPolyDataFilter> prepared;
    prepared->SetInputData(source);
    prepared->SetTransform(start);
    prepared->Update();
    vtkNew<vtkIterativeClosestPointTransform> fit;
    fit->SetSource(prepared->GetOutput());
    fit->SetTarget(target);
    if (allowScale) fit->GetLandmarkTransform()->SetModeToSimilarity();
    else fit->GetLandmarkTransform()->SetModeToRigidBody();
    fit->SetMaximumNumberOfIterations(80);
    fit->SetMaximumNumberOfLandmarks(5000);
    fit->SetMaximumMeanDistance(0.001);
    fit->CheckMeanDistanceOn();
    if (!initial) fit->StartByMatchingCentroidsOn();
    fit->Update();
    if (initial) vtkMatrix4x4::Multiply4x4(fit->GetMatrix(), initial, matrix);
    else matrix->DeepCopy(fit->GetMatrix());
    const double scale = uniformScale(matrix);
    if (!std::isfinite(scale) || scale < 1e-12) {
        if (error) *error = QStringLiteral("ICP produced an invalid scale.");
        return false;
    }
    if (iterations) *iterations = fit->GetNumberOfIterations();
    return true;
}

RegistrationMetrics RegistrationEngine::measure(vtkPolyData *source, vtkPolyData *target,
                                                vtkMatrix4x4 *matrix, double thresholdMeters) {
    RegistrationMetrics metrics;
    if (!source || !target || !matrix || source->GetNumberOfPoints() == 0 ||
        target->GetNumberOfCells() == 0) return metrics;
    vtkNew<vtkStaticCellLocator> locator;
    locator->SetDataSet(target);
    locator->BuildLocator();
    vtkNew<vtkGenericCell> cell;
    const vtkIdType step = std::max<vtkIdType>(1, source->GetNumberOfPoints() / 10000);
    double squared = 0;
    int inliers = 0;
    for (vtkIdType i = 0; i < source->GetNumberOfPoints(); i += step) {
        double point[3], transformed[4], closest[3], distanceSquared;
        vtkIdType cellId;
        int subId;
        source->GetPoint(i, point);
        const double input[4]{point[0], point[1], point[2], 1.0};
        matrix->MultiplyPoint(input, transformed);
        locator->FindClosestPoint(transformed, closest, cell, cellId, subId, distanceSquared);
        if (distanceSquared <= thresholdMeters * thresholdMeters) {
            squared += distanceSquared;
            ++inliers;
        }
        ++metrics.samples;
    }
    metrics.overlap = double(inliers) / metrics.samples;
    metrics.rmse = inliers ? std::sqrt(squared / inliers) : 0.0;
    return metrics;
}

QVector3D RegistrationEngine::transformPoint(vtkMatrix4x4 *matrix, const QVector3D &point) {
    double p[4];
    const double input[4]{point.x(), point.y(), point.z(), 1.0};
    matrix->MultiplyPoint(input, p);
    return QVector3D(float(p[0]), float(p[1]), float(p[2]));
}

double RegistrationEngine::uniformScale(vtkMatrix4x4 *matrix) {
    double length = 0;
    for (int row = 0; row < 3; ++row)
        length += std::pow(matrix->GetElement(row, 0), 2);
    return std::sqrt(length);
}

QQuaternion RegistrationEngine::transformOrientation(vtkMatrix4x4 *matrix,
                                                       const QQuaternion &orientation) {
    QMatrix3x3 rotation;
    const double scale = uniformScale(matrix);
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            rotation(row, column) = float(matrix->GetElement(row, column) / scale);
    return (QQuaternion::fromRotationMatrix(rotation) * orientation).normalized();
}
