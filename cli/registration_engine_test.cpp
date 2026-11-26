#include "registration_engine.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cmath>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPolyData.h>

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    QTemporaryDir sample;
    QFile obj(sample.filePath(QStringLiteral("classified.obj")));
    if (!sample.isValid() || !obj.open(QIODevice::WriteOnly)) return 10;
    obj.write("v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nv 1 0 1\nv 0 1 1\n"
              "g Part-EX-FR-01\nf 1 2 3\ng Part-IN-FR-01\nf 4 5 6\n");
    obj.close();
    QVector<RegistrationPart> classified;
    vtkNew<vtkPolyData> combined;
    QString classifiedError;
    if (!RegistrationEngine::loadModel(sample.path(), 1.0, &classified, combined, &classifiedError) ||
        classified.size() != 1 || !classified[0].exteriorFaces || !classified[0].interiorFaces ||
        classified[0].exteriorFaces->GetNumberOfPolys() != 1 ||
        classified[0].interiorFaces->GetNumberOfPolys() != 1) return 11;
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
    if (!RegistrationEngine::icp(sourceCloud, targetCloud, rigid, false, refined, &iterations, &error))
        return 3;
    for (int i = 0; i < source.size(); ++i)
        if ((RegistrationEngine::transformPoint(refined, source[i]) - target[i]).length() > 1e-4)
            return 4;
    const auto metrics = RegistrationEngine::measure(sourceCloud, targetCloud, refined, 0.1);
    if (metrics.overlap < 0.99 || metrics.rmse > 1e-4) return 5;
    OrientationComparison compared;
    if (!RegistrationEngine::compareOrientations(sourceCloud, targetCloud, rigid, false,
                                                  &compared, &error) || compared.candidates.size() != 2 ||
        compared.candidates[0].angleDegrees != 0 || compared.candidates[1].angleDegrees != 180 ||
        compared.candidates[0].metrics.overlap < 0.99 ||
        compared.candidates[0].metrics.rmse > 1e-4) return 16;
    const QVector<QVector3D> symmetricPoints{{-1, -1, 0}, {-1, 1, 0}, {1, -1, 0}, {1, 1, 0},
                                             {-1, -1, 2}, {-1, 1, 2}, {1, -1, 2}, {1, 1, 2}};
    auto symmetricCloud = RegistrationEngine::cloudGeometry(symmetricPoints);
    if (!RegistrationEngine::compareOrientations(symmetricCloud, symmetricCloud, nullptr, false,
                                                  &compared, &error) ||
        compared.candidates.size() != 2 || compared.suggestedIndex != -1) return 17;
    auto scaledCloud = RegistrationEngine::cloudGeometry(scaled);
    vtkNew<vtkMatrix4x4> nearSimilarity;
    nearSimilarity->DeepCopy(similarity);
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            nearSimilarity->SetElement(row, column, nearSimilarity->GetElement(row, column) * 0.9);
    vtkNew<vtkMatrix4x4> fixedScale;
    if (!RegistrationEngine::icp(sourceCloud, scaledCloud, nearSimilarity, false,
                                 fixedScale, &iterations, &error) ||
        std::abs(RegistrationEngine::uniformScale(fixedScale) - 1.8) > 1e-4) return 13;
    vtkNew<vtkMatrix4x4> fittedScale;
    if (!RegistrationEngine::icp(sourceCloud, scaledCloud, nearSimilarity, true,
                                 fittedScale, &iterations, &error) ||
        std::abs(RegistrationEngine::uniformScale(fittedScale) - 2.0) > 1e-3) return 14;
    for (int i = 0; i < source.size(); ++i)
        if ((RegistrationEngine::transformPoint(fittedScale, source[i]) - scaled[i]).length() > 1e-3)
            return 15;
    if (argc > 1) {
        QVector<RegistrationPart> parts;
        vtkNew<vtkPolyData> model;
        if (!RegistrationEngine::loadModel(QString::fromLocal8Bit(argv[1]), 0.001,
                                           &parts, model, &error)) return 6;
        if (parts.size() != 26 || model->GetNumberOfPolys() < 2000) return 7;
        int exteriorParts = 0, interiorParts = 0;
        for (const auto &part : parts) {
            if (part.exteriorFaces && part.exteriorFaces->GetNumberOfPolys() > 0) ++exteriorParts;
            if (part.interiorFaces && part.interiorFaces->GetNumberOfPolys() > 0) ++interiorParts;
        }
        if (exteriorParts < 20 || interiorParts < 15) return 12;
        double bounds[6];
        model->GetBounds(bounds);
        if (std::abs(bounds[0] + 15.0) > 0.01 || std::abs(bounds[1] - 15.0) > 0.01 ||
            std::abs(bounds[4] - (-1.551)) > 0.01 || std::abs(bounds[5] - 83.149) > 0.01)
            return 9;
    }
    return 0;
}
