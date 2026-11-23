#include "registration_engine.h"

#include <QCoreApplication>
#include <cmath>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPolyData.h>

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    const QVector<QVector3D> source{{0, 0, 0}, {1, 0, 0}, {0, 1, 0},
                                     {0, 0, 1}, {1, 1, 1}};
    QVector<QVector3D> target;
    for (const auto &point : source)
        target.append(QVector3D(-point.y() + 3, point.x() - 2, point.z() + 5));
    vtkNew<vtkMatrix4x4> rigid;
    double rmse = -1;
    QString error;
    if (!RegistrationEngine::landmarks(source, target, false, rigid, &rmse, &error) ||
        rmse > 1e-5 || std::abs(RegistrationEngine::uniformScale(rigid) - 1) > 1e-5)
        return 1;
    const auto facing = RegistrationEngine::transformOrientation(rigid, QQuaternion())
        .rotatedVector(QVector3D(1, 0, 0));
    if ((facing - QVector3D(0, 1, 0)).length() > 1e-4) return 8;
    QVector<QVector3D> scaled;
    for (const auto &point : target) scaled.append(point * 2);
    vtkNew<vtkMatrix4x4> similarity;
    if (!RegistrationEngine::landmarks(source, scaled, true, similarity, &rmse, &error) ||
        rmse > 1e-5 || std::abs(RegistrationEngine::uniformScale(similarity) - 2) > 1e-5)
        return 2;
    auto sourceCloud = RegistrationEngine::cloudGeometry(source);
    auto targetCloud = RegistrationEngine::cloudGeometry(target);
    vtkNew<vtkMatrix4x4> refined;
    int iterations = 0;
    if (!RegistrationEngine::icp(sourceCloud, targetCloud, rigid, refined, &iterations, &error))
        return 3;
    for (int i = 0; i < source.size(); ++i)
        if ((RegistrationEngine::transformPoint(refined, source[i]) - target[i]).length() > 1e-4)
            return 4;
    const auto metrics = RegistrationEngine::measure(sourceCloud, targetCloud, refined, 0.1);
    if (metrics.overlap < 0.99 || metrics.rmse > 1e-4) return 5;
    if (argc > 1) {
        QVector<RegistrationPart> parts;
        vtkNew<vtkPolyData> model;
        if (!RegistrationEngine::loadModel(QString::fromLocal8Bit(argv[1]), 0.001,
                                           &parts, model, &error)) return 6;
        if (parts.size() != 26 || model->GetNumberOfPolys() < 2000) return 7;
        double bounds[6];
        model->GetBounds(bounds);
        if (std::abs(bounds[0] + 15.0) > 0.01 || std::abs(bounds[1] - 15.0) > 0.01 ||
            std::abs(bounds[4] - (-1.551)) > 0.01 || std::abs(bounds[5] - 83.149) > 0.01)
            return 9;
    }
    return 0;
}
