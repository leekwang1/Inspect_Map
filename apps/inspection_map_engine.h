#pragma once

#include <QHash>
#include <QList>
#include <QMatrix4x4>
#include <QPointF>
#include <QQuaternion>
#include <QSet>
#include <QString>
#include <QVector>
#include <QVector3D>

struct MapTriangle {
    QVector3D a, b, c;
    QString partId;
    QString faceId;
};

struct MapDrawing {
    QString id;
    QString partId;
    QString path;
    QSet<QString> faceIds;
    QVector<QVector<QVector3D>> lines;
    int horizontalAxis = 0;
    int verticalAxis = 2;
};

struct MapHit {
    bool found = false;
    QVector3D point;
    QString partId;
    QString faceId;
    double distance = 0;
};

struct UnrollTriangle {
    MapTriangle source;
    QPointF a, b, c;
};

struct UnrollFace {
    QString faceId;
    QString partId;
    QString direction;
    QVector<QPointF> outline;
    QVector<UnrollTriangle> triangles;
};

struct UnrollSheet {
    QString id;
    QString series;
    QString side;
    QVector<UnrollFace> faces;
    QStringList parts;
    bool point(const QString &faceId, const QVector3D &world, QPointF *unrolled) const;
};

class InspectionMapEngine {
public:
    bool load(const QString &root, QString *error);
    MapHit cast(const QVector3D &origin, const QVector3D &direction) const;
    QStringList parts() const;
    const QList<MapDrawing> &drawings() const { return drawings_; }
    const MapDrawing *drawing(const QString &id) const;
    const MapDrawing *drawingForFace(const QString &faceId) const;
    const QList<UnrollSheet> &unrollSheets() const { return unrollSheets_; }
    const UnrollSheet *unrollSheet(const QString &id) const;
    static QPointF drawingPoint(const MapDrawing &drawing, const QVector3D &point);
    static QVector3D cameraRay(const QVector3D &pixel, double fx, double fy, double cx, double cy,
                               const QQuaternion &orientation);
    int triangleCount() const { return triangles_.size(); }

private:
    QVector<MapTriangle> triangles_;
    QList<MapDrawing> drawings_;
    QHash<QString, int> faceDrawing_;
    QList<UnrollSheet> unrollSheets_;
};
