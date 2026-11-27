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
    const bool fallbackOnly = QString::fromLocal8Bit(argv[1]) == QStringLiteral("--filename-fallback");
    QString error;
    if (!fallbackOnly) {
    InspectionMapEngine engine;
    if (!engine.load(QString::fromLocal8Bit(argv[1]), &error)) {
        qCritical().noquote() << error;
        return 2;
    }
    if (engine.parts().size() < 20 || engine.triangleCount() < 1000 ||
        engine.drawings().size() < 100) return 3;
    const auto hit = engine.cast(QVector3D(3, -5, 22.649f), QVector3D(0, 1, 0));
    if (!hit.found || hit.partId != QStringLiteral("SP-PY3-BRG-01") ||
        hit.faceId != QStringLiteral("SP-PY3-BRG-01-EX-FR-01") ||
        qAbs(hit.point.y() + 2.5f) > 0.001f) return 4;
    const auto *drawing = engine.drawingForFace(hit.faceId);
    if (!drawing || drawing->id != QStringLiteral("SP-PY3-BRG-01-EX-FR") ||
        drawing->lines.isEmpty()) return 5;
    const auto location = InspectionMapEngine::drawingPoint(*drawing, hit.point);
    if (qAbs(location.x() - 3) > 0.001 || qAbs(location.y() + 22.649) > 0.001) return 6;
    const auto forward = InspectionMapEngine::cameraRay(QVector3D(100, 100, 0), 100, 100, 100, 100, {});
    if ((forward - QVector3D(1, 0, 0)).length() > 0.001f) return 7;
    const auto *sheet = engine.unrollSheet(QStringLiteral("SP-PY3-LSH|EX"));
    const auto *inside = engine.unrollSheet(QStringLiteral("SP-PY3-LSH|IN"));
    if (!sheet || sheet->faces.size() != 152 || sheet->parts.size() != 7 ||
        !inside || inside->faces.size() != 48 || inside->parts.size() != 7) return 10;
    const auto &mapped = sheet->faces.first().triangles.first();
    QPointF mappedPoint;
    if (!sheet->point(sheet->faces.first().faceId, mapped.source.a, &mappedPoint) ||
        (mappedPoint-mapped.a).manhattanLength() > 0.001) return 11;
    qInfo() << "Parts" << engine.parts().size() << "faces" << engine.triangleCount()
            << "drawings" << engine.drawings().size() << "unroll sheets" << engine.unrollSheets().size()
            << "LSH exterior faces" << sheet->faces.size();
    }
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
    if (!generated || generated->faces.size() != 4) return 16;
    double minX = 1e9, maxX = -1e9;
    for (const auto &face : generated->faces) for (const auto &point : face.outline) {
        minX = std::min(minX, point.x()); maxX = std::max(maxX, point.x());
    }
    if (qAbs(maxX-minX-4.0) > 0.001) return 17;
    if (!QDir().mkpath(temporary.filePath(QStringLiteral("model/NAMED"))) ||
        !QDir().mkpath(temporary.filePath(QStringLiteral("map/NAMED")))) return 19;
    QFile namedMetadata(temporary.filePath(QStringLiteral("model/NAMED/meta.gxxml")));
    if (!namedMetadata.open(QIODevice::WriteOnly) || namedMetadata.write(
        "<Softgraphy><Transform sx='1' sy='1' sz='1'/></Softgraphy>") < 0) return 20;
    namedMetadata.close();
    QFile namedObject(temporary.filePath(QStringLiteral("model/NAMED/NAMED.obj")));
    if (!namedObject.open(QIODevice::WriteOnly) || namedObject.write(
        "v 0 0 0\nv 2 0 0\nv 2 0 2\nv 0 0 2\n"
        "v 0 1 2\nv 2 1 2\n"
        "g NAMED-IN-FR-01\nf 1 2 3\nf 1 3 4\n"
        "g NAMED-CAP-TP-01\nf 4 3 6\nf 4 6 5\n") < 0) return 21;
    namedObject.close();
    const QByteArray drawingDxf =
        "0\nSECTION\n2\nENTITIES\n0\nPOLYLINE\n"
        "0\nVERTEX\n10\n0\n20\n0\n30\n0\n"
        "0\nVERTEX\n10\n2\n20\n0\n30\n0\n"
        "0\nVERTEX\n10\n2\n20\n0\n30\n2\n"
        "0\nVERTEX\n10\n0\n20\n0\n30\n2\n"
        "0\nSEQEND\n0\nENDSEC\n0\nEOF\n";
    for (const auto &name : {"NAMED-IN-FR", "NAMED-TP"}) {
        QFile map(temporary.filePath(QStringLiteral("map/NAMED/") + QLatin1StringView(name) + QStringLiteral(".dxf")));
        if (!map.open(QIODevice::WriteOnly) || map.write(drawingDxf) < 0) return 22;
    }
    InspectionMapEngine inferred;
    if (!inferred.load(temporary.path(), &error)) return 23;
    const auto *front = inferred.drawingForFace(QStringLiteral("NAMED-IN-FR-01"));
    const auto *top = inferred.drawingForFace(QStringLiteral("NAMED-CAP-TP-01"));
    if (!front || front->id != QStringLiteral("NAMED-IN-FR") ||
        !top || top->id != QStringLiteral("NAMED-TP") ||
        !front->faceIds.contains(QStringLiteral("NAMED-IN-FR-01")) ||
        !top->faceIds.contains(QStringLiteral("NAMED-CAP-TP-01")) ||
        inferred.drawingForFace(QStringLiteral("NAMED-IN-FR-99"))) return 24;
    return 0;
}
