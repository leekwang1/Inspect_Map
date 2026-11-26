#define main registration_tool_entry
#include "../apps/registration_tool.cpp"
#undef main

#include <QTemporaryDir>
#include <vtkActorCollection.h>

namespace {
vtkMatrix4x4 *previewMatrix(PickView *view) {
    auto *actors = view->renderer()->GetActors();
    actors->InitTraversal();
    while (auto *actor = actors->GetNextActor())
        if (actor->GetUserMatrix()) return actor->GetUserMatrix();
    return nullptr;
}
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    if (argc < 2) return 1;
    QTemporaryDir temp;
    if (!temp.isValid()) return 2;
    const QString dbPath = QDir(temp.path()).filePath(QStringLiteral("project.sqlite"));
    if (!QFile::copy(QString::fromLocal8Bit(argv[1]), dbPath)) return 3;
    RegistrationWindow window(dbPath, {}, {}, {}, {}, {});
    window.show();
    app.processEvents();
    const auto widgets = window.findChildren<QVTKOpenGLNativeWidget *>();
    const auto lists = window.findChildren<QListWidget *>();
    const auto tables = window.findChildren<QTableWidget *>();
    if (tables.size() < 2) return 8;
    for (auto *table : tables)
        if (!dynamic_cast<RowSelectionDelegate *>(table->itemDelegate()) ||
            !table->styleSheet().contains(QStringLiteral("outline:none"))) return 9;
    if (widgets.size() != 3 || lists.isEmpty() || lists.first()->count() == 0) return 4;
    auto *resultView = static_cast<PickView *>(widgets[2]);
    auto *matrix = previewMatrix(resultView);
    if (!matrix) return 5;
    const double expected = matrix->GetElement(0, 0);
    auto *camera = resultView->renderer()->GetActiveCamera();
    camera->Zoom(1.8);
    const double zoomedAngle = camera->GetViewAngle();
    double position[3], focal[3];
    camera->GetPosition(position);
    camera->GetFocalPoint(focal);
    const auto cameraUnchanged = [&] {
        if (std::abs(camera->GetViewAngle() - zoomedAngle) > 1e-9) return false;
        for (int axis = 0; axis < 3; ++axis)
            if (std::abs(camera->GetPosition()[axis] - position[axis]) > 1e-9 ||
                std::abs(camera->GetFocalPoint()[axis] - focal[axis]) > 1e-9) return false;
        return true;
    };
    lists.first()->item(0)->setCheckState(Qt::Checked);
    app.processEvents();
    matrix = previewMatrix(resultView);
    if (!matrix || std::abs(matrix->GetElement(0, 0) - expected) > 1e-9 || !cameraUnchanged()) return 6;
    lists.first()->item(0)->setCheckState(Qt::Unchecked);
    app.processEvents();
    matrix = previewMatrix(resultView);
    if (!matrix || std::abs(matrix->GetElement(0, 0) - expected) > 1e-9 || !cameraUnchanged()) return 7;
    return 0;
}
