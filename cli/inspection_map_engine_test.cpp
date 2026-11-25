#include "inspection_map_engine.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtMath>

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    if (argc < 2) return 1;
    InspectionMapEngine engine;
    QString error;
    if (!engine.load(QString::fromLocal8Bit(argv[1]), &error)) {
        qCritical().noquote() << error;
        return 2;
    }
    if (engine.parts().size() != 26 || engine.triangleCount() < 1000 ||
        engine.drawings().size() < 240) return 3;
    const auto hit = engine.cast(QVector3D(3, -5, 22.649f), QVector3D(0, 1, 0));
    if (!hit.found || hit.partId != QStringLiteral("SP-PY3-BRG-01") ||
        hit.faceId != QStringLiteral("SP-PY3-BRG-01-EX-FR-01") ||
        qAbs(hit.point.y() + 2.5f) > 0.001f) return 4;
    const auto *drawing = engine.drawingForFace(hit.faceId);
    if (!drawing || drawing->id != QStringLiteral("SP-PY3-BRG-01-EX-FR") ||
        drawing->lines.isEmpty()) return 5;
    const auto location = InspectionMapEngine::drawingPoint(*drawing, hit.point);
    if (qAbs(location.x() - 3) > 0.001 || qAbs(location.y() + 22.649) > 0.001) return 6;
    QVector3D recovered;
    if (!engine.mapPoint(drawing->id, hit.faceId, location, &recovered) ||
        (recovered-hit.point).length() > 0.01f ||
        engine.mapPoint(drawing->id, hit.faceId, QPointF(1e6, 1e6), &recovered)) return 18;
    const auto forward = InspectionMapEngine::cameraRay(QVector3D(100, 100, 0), 100, 100, 100, 100, {});
    if ((forward - QVector3D(1, 0, 0)).length() > 0.001f) return 7;
    const auto chart = engine.unroll(QStringLiteral("SP-PY3-LSH-R04"));
    if (chart.strips.isEmpty() || chart.exteriorWidth <= 0 || chart.excludedFaces.isEmpty()) return 8;
    QPointF first, second;
    const auto &strip = chart.strips.first();
    if (!chart.point(strip.faceId, strip.start, &first) ||
        !chart.point(strip.faceId, strip.end, &second) ||
        qAbs(second.x() - first.x() - strip.width) > 0.001 ||
        qAbs(first.y() + strip.start.z()) > 0.001) return 9;
    const auto *sheet = engine.unrollSheet(QStringLiteral("SP-PY3-LSH|EX"));
    if (!sheet || sheet->faces.size() < 100 || sheet->parts.size() != 6) return 10;
    const auto &mapped = sheet->faces.first().triangles.first();
    QPointF mappedPoint;
    if (!sheet->point(sheet->faces.first().faceId, mapped.source.a, &mappedPoint) ||
        (mappedPoint-mapped.a).manhattanLength() > 0.001) return 11;
    QTemporaryDir temporary;
    if (!temporary.isValid() || !QDir().mkpath(temporary.filePath(QStringLiteral("model/BOX")))) return 12;
    QFile metadata(temporary.filePath(QStringLiteral("model/BOX/meta.gxxml")));
    if (!metadata.open(QIODevice::WriteOnly) || metadata.write(
        "<Softgraphy><Transform tx='0' ty='0' tz='0' sx='1' sy='1' sz='1'/></Softgraphy>") < 0) return 13;
    metadata.close();
    QFile object(temporary.filePath(QStringLiteral("model/BOX/BOX.obj")));
    if (!object.open(QIODevice::WriteOnly) || object.write(
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "v 0 0 2\nv 1 0 2\nv 1 1 2\nv 0 1 2\n"
        "g FR\nf 1 2 5\nf 2 6 5\n"
        "g RG\nf 2 3 6\nf 3 7 6\n"
        "g RR\nf 3 4 7\nf 4 8 7\n"
        "g LF\nf 4 1 8\nf 1 5 8\n") < 0) return 14;
    object.close();
    InspectionMapEngine modelOnly;
    if (!modelOnly.load(temporary.path(), &error)) return 15;
    const auto *generated = modelOnly.unrollSheet(QStringLiteral("BOX|GEN"));
    if (!generated || generated->sourceDxf || generated->faces.size() != 4) return 16;
    double minX = 1e9, maxX = -1e9;
    for (const auto &face : generated->faces) for (const auto &point : face.outline) {
        minX = std::min(minX, point.x()); maxX = std::max(maxX, point.x());
    }
    if (qAbs(maxX-minX-4.0) > 0.001) return 17;
    qInfo() << "Parts" << engine.parts().size() << "faces" << engine.triangleCount()
            << "drawings" << engine.drawings().size() << "unroll sheets" << engine.unrollSheets().size()
            << "LSH exterior faces" << sheet->faces.size();
    return 0;
}
