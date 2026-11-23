#pragma once

#include <QQuaternion>
#include <QString>
#include <QVector>
#include <QVector3D>
#include <vtkSmartPointer.h>

class vtkMatrix4x4;
class vtkPolyData;

struct RegistrationPart {
    QString id;
    vtkSmartPointer<vtkPolyData> geometry;
};

struct RegistrationMetrics {
    double rmse = 0.0;
    double overlap = 0.0;
    int samples = 0;
};

class RegistrationEngine {
public:
    static bool loadModel(const QString &root, double fallbackScale,
                          QVector<RegistrationPart> *parts, vtkPolyData *combined, QString *error);
    static vtkSmartPointer<vtkPolyData> cloudGeometry(const QVector<QVector3D> &points);
    static bool landmarks(const QVector<QVector3D> &source, const QVector<QVector3D> &target,
                          bool allowScale, vtkMatrix4x4 *matrix, double *rmse, QString *error);
    static bool icp(vtkPolyData *source, vtkPolyData *target, vtkMatrix4x4 *initial,
                    vtkMatrix4x4 *matrix, int *iterations, QString *error);
    static RegistrationMetrics measure(vtkPolyData *source, vtkPolyData *target,
                                       vtkMatrix4x4 *matrix, double thresholdMeters = 0.5);
    static QVector3D transformPoint(vtkMatrix4x4 *matrix, const QVector3D &point);
    static QQuaternion transformOrientation(vtkMatrix4x4 *matrix, const QQuaternion &orientation);
    static double uniformScale(vtkMatrix4x4 *matrix);
};
