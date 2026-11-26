#include "lidar_preview.h"
#include "registration_engine.h"
#include "project_store.h"
#include "conversion_core.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointF>
#include <QPainter>
#include <QPushButton>
#include <QRect>
#include <QRubberBand>
#include <QDoubleSpinBox>
#include <QMatrix4x4>
#include <QScrollArea>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolButton>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <optional>
#include <functional>
#include <vtkActor.h>
#include <vtkAppendPolyData.h>
#include <vtkBillboardTextActor3D.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCellPicker.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkLookupTable.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkPointPicker.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSphereSource.h>
#include <vtkTextProperty.h>
#include <vtkUnsignedShortArray.h>
#include <cmath>

namespace {
class RowSelectionDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem rowOption(option);
        rowOption.state &= ~QStyle::State_HasFocus;
        QStyledItemDelegate::paint(painter, rowOption, index);
    }
};

QString id() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

struct CameraPreviewItem {
    QString name;
    PreviewCamera pose;
    int width = 0;
    int height = 0;
};

struct PreviewIntrinsics {
    double fx = 0, fy = 0, cx = 0, cy = 0;
    int width = 0, height = 0;
    bool valid = false;
};

PreviewIntrinsics readPreviewIntrinsics(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    const auto rows = object.value(QStringLiteral("P")).toArray();
    const auto size = object.value(QStringLiteral("image_size")).toObject();
    if (rows.size() < 2 || rows[0].toArray().size() < 3 || rows[1].toArray().size() < 3) return {};
    PreviewIntrinsics result;
    result.fx = rows[0].toArray()[0].toDouble();
    result.fy = rows[1].toArray()[1].toDouble();
    result.cx = rows[0].toArray()[2].toDouble();
    result.cy = rows[1].toArray()[2].toDouble();
    result.width = size.value(QStringLiteral("width")).toInt();
    result.height = size.value(QStringLiteral("height")).toInt();
    result.valid = result.fx > 0 && result.fy > 0 && result.width > 0 && result.height > 0;
    return result;
}

vtkSmartPointer<vtkActor> cameraCentersActor(const QVector<QVector3D> &positions) {
    vtkNew<vtkPoints> points;
    vtkNew<vtkCellArray> vertices;
    for (const auto &position : positions) {
        const vtkIdType index = points->InsertNextPoint(position.x(), position.y(), position.z());
        vertices->InsertNextCell(1);
        vertices->InsertCellPoint(index);
    }
    vtkNew<vtkPolyData> geometry;
    geometry->SetPoints(points);
    geometry->SetVerts(vertices);
    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputData(geometry);
    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetColor(0.95, 0.29, 0.23);
    actor->GetProperty()->SetPointSize(7.0);
    actor->GetProperty()->LightingOff();
    actor->PickableOff();
    return actor;
}

vtkSmartPointer<vtkActor> cameraFrustumActor(const QVector3D &origin, const QQuaternion &orientation,
                                            const PreviewIntrinsics &intrinsics, int width, int height,
                                            double length) {
    const QVector3D forward = orientation.rotatedVector(QVector3D(1, 0, 0));
    const QVector3D right = orientation.rotatedVector(QVector3D(0, 1, 0));
    const QVector3D up = orientation.rotatedVector(QVector3D(0, 0, 1));
    QVector<QVector3D> corners;
    if (intrinsics.valid && width > 0 && height > 0) {
        const double scaleX = double(width) / intrinsics.width;
        const double scaleY = double(height) / intrinsics.height;
        const double fx = intrinsics.fx * scaleX, fy = intrinsics.fy * scaleY;
        const double cx = intrinsics.cx * scaleX, cy = intrinsics.cy * scaleY;
        for (const auto &pixel : {QPointF(0, 0), QPointF(width, 0),
                                  QPointF(width, height), QPointF(0, height)}) {
            corners.append(origin + forward * float(length) +
                right * float((pixel.x() - cx) / fx * length) -
                up * float((pixel.y() - cy) / fy * length));
        }
    } else {
        const float halfWidth = float(length * 0.65), halfHeight = halfWidth * 0.75f;
        const QVector3D target = origin + forward * float(length);
        corners = {target - right * halfWidth + up * halfHeight,
                   target + right * halfWidth + up * halfHeight,
                   target + right * halfWidth - up * halfHeight,
                   target - right * halfWidth - up * halfHeight};
    }
    vtkNew<vtkPoints> points;
    points->InsertNextPoint(origin.x(), origin.y(), origin.z());
    for (const auto &point : corners) points->InsertNextPoint(point.x(), point.y(), point.z());
    vtkNew<vtkCellArray> lines;
    for (vtkIdType i = 1; i <= 4; ++i) {
        lines->InsertNextCell(2);
        lines->InsertCellPoint(0);
        lines->InsertCellPoint(i);
        lines->InsertNextCell(2);
        lines->InsertCellPoint(i);
        lines->InsertCellPoint(i % 4 + 1);
    }
    vtkNew<vtkPolyData> geometry;
    geometry->SetPoints(points);
    geometry->SetLines(lines);
    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputData(geometry);
    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetColor(1.0, 0.74, 0.22);
    actor->GetProperty()->SetLineWidth(2.0);
    actor->GetProperty()->LightingOff();
    actor->PickableOff();
    return actor;
}

vtkSmartPointer<vtkActor> modelActor(vtkPolyData *geometry, double opacity) {
    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputData(geometry);
    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetColor(0.73, 0.79, 0.82);
    actor->GetProperty()->SetOpacity(opacity);
    return actor;
}

vtkSmartPointer<vtkActor> cloudActor(vtkPolyData *geometry, const CloudPreview &cloud) {
    vtkNew<vtkUnsignedShortArray> intensity;
    intensity->SetName("Intensity");
    for (auto value : cloud.intensities) intensity->InsertNextValue(value);
    geometry->GetPointData()->SetScalars(intensity);
    vtkNew<vtkLookupTable> colors;
    colors->SetNumberOfTableValues(256);
    colors->SetRange(cloud.intensityLow, std::max<int>(cloud.intensityLow + 1, cloud.intensityHigh));
    for (int i = 0; i < 256; ++i) {
        const double t = double(i) / 255;
        colors->SetTableValue(i, 0.12 + 0.86 * t, 0.42 + 0.46 * t, 0.76 - 0.47 * t, 1);
    }
    colors->Build();
    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputData(geometry);
    mapper->SetLookupTable(colors);
    mapper->UseLookupTableScalarRangeOn();
    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetPointSize(2.0);
    return actor;
}

class PickView : public QVTKOpenGLNativeWidget {
public:
    explicit PickView(bool pickSurface, QWidget *parent = nullptr)
        : QVTKOpenGLNativeWidget(parent), pickSurface_(pickSurface) {
        window_ = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
        renderer_ = vtkSmartPointer<vtkRenderer>::New();
        renderer_->SetBackground(0.07, 0.13, 0.17);
        window_->AddRenderer(renderer_);
        setRenderWindow(window_);
        setMinimumSize(280, 300);
        rubberBand_ = new QRubberBand(QRubberBand::Rectangle, this);
        rubberBand_->setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    vtkRenderer *renderer() const { return renderer_; }
    void setPickActor(vtkActor *actor) { pickActor_ = actor; }
    void arm(std::function<void(const QVector3D &)> callback) {
        clearArm();
        callback_ = std::move(callback);
        setCursor(Qt::CrossCursor);
    }
    void armRectangle(std::function<void(const QRect &)> callback) {
        clearArm();
        rectangleCallback_ = std::move(callback);
        rectangleCamera_ = vtkSmartPointer<vtkCamera>::New();
        rectangleCamera_->DeepCopy(renderer_->GetActiveCamera());
        if (auto *interactor = window_->GetInteractor(); interactor && interactor->GetEnabled()) {
            interactor->Disable();
            rectangleDisabledInteractor_ = true;
        }
        setCursor(Qt::CrossCursor);
    }
    void clearArm() {
        callback_ = {};
        rectangleCallback_ = {};
        draggingRectangle_ = false;
        rubberBand_->hide();
        if (rectangleCamera_) {
            renderer_->GetActiveCamera()->DeepCopy(rectangleCamera_);
            rectangleCamera_ = nullptr;
            renderer_->ResetCameraClippingRange();
            refresh();
        }
        if (rectangleDisabledInteractor_) {
            if (auto *interactor = window_->GetInteractor()) interactor->Enable();
            rectangleDisabledInteractor_ = false;
        }
        unsetCursor();
    }
    void refresh() { if (isValid()) window_->Render(); else update(); }
    void fit() {
        double bounds[6]; renderer_->ComputeVisiblePropBounds(bounds);
        fitBounds(bounds);
    }
    void fitGeometry(vtkPolyData *geometry) {
        if (!geometry) return;
        double bounds[6]; geometry->GetBounds(bounds);
        fitBounds(bounds);
    }
    void setPivot(const QVector3D &point) {
        auto *camera = renderer_->GetActiveCamera();
        const auto *old = camera->GetFocalPoint();
        const auto *position = camera->GetPosition();
        const double offset[3]{position[0] - old[0], position[1] - old[1], position[2] - old[2]};
        camera->SetFocalPoint(point.x(), point.y(), point.z());
        camera->SetPosition(point.x() + offset[0], point.y() + offset[1], point.z() + offset[2]);
        viewInitialized_ = true;
        renderer_->ResetCameraClippingRange();
        refresh();
    }
    void axisView(int axis) {
        auto *camera = renderer_->GetActiveCamera();
        const auto *center = camera->GetFocalPoint();
        const double distance = std::max(camera->GetDistance(), 1.0);
        camera->SetPosition(center[0] + (axis == 0 ? distance : 0),
                            center[1] + (axis == 1 ? distance : 0),
                            center[2] + (axis == 2 ? distance : 0));
        camera->SetViewUp(0, axis == 2 ? 1 : 0, axis == 2 ? 0 : 1);
        camera->ParallelProjectionOn();
        viewInitialized_ = true;
        renderer_->ResetCameraClippingRange();
        refresh();
    }
    std::optional<QPointF> displayPoint(const QVector3D &point) const {
        renderer_->SetWorldPoint(point.x(), point.y(), point.z(), 1.0);
        renderer_->WorldToDisplay();
        const auto *display = renderer_->GetDisplayPoint();
        if (!std::isfinite(display[0]) || !std::isfinite(display[1]) ||
            display[2] < 0.0 || display[2] > 1.0) return std::nullopt;
        const double scale = devicePixelRatioF();
        return QPointF(display[0] / scale, (window_->GetSize()[1] - 1 - display[1]) / scale);
    }

private:
    void fitBounds(const double bounds[6]) {
        renderer_->ResetCamera(bounds);
        if (!viewInitialized_) {
            const double extent = std::max({bounds[1] - bounds[0], bounds[3] - bounds[2],
                                            bounds[5] - bounds[4], 1.0});
            const double cx = (bounds[0] + bounds[1]) * 0.5;
            const double cy = (bounds[2] + bounds[3]) * 0.5;
            const double cz = (bounds[4] + bounds[5]) * 0.5;
            auto *camera = renderer_->GetActiveCamera();
            camera->SetFocalPoint(cx, cy, cz);
            camera->SetPosition(cx + extent * 1.8, cy - extent * 2.5, cz + extent * 0.7);
            camera->SetViewUp(0, 0, 1);
            viewInitialized_ = true;
        }
        renderer_->ResetCameraClippingRange();
        refresh();
    }

protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && rectangleCallback_) {
            pressPosition_ = event->position();
            draggingRectangle_ = true;
            rubberBand_->setGeometry(QRect(pressPosition_.toPoint(), QSize()));
            rubberBand_->show();
            event->accept();
            return;
        }
        if (event->button() == Qt::LeftButton && callback_) pressPosition_ = event->position();
        QVTKOpenGLNativeWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (draggingRectangle_) {
            rubberBand_->setGeometry(QRect(pressPosition_.toPoint(), event->position().toPoint()).normalized());
            event->accept();
            return;
        }
        QVTKOpenGLNativeWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && draggingRectangle_) {
            const QRect area(pressPosition_.toPoint(), event->position().toPoint());
            auto callback = std::move(rectangleCallback_);
            clearArm();
            if (area.normalized().width() > 6 && area.normalized().height() > 6)
                callback(area.normalized());
            event->accept();
            return;
        }
        QVTKOpenGLNativeWidget::mouseReleaseEvent(event);
        if (event->button() != Qt::LeftButton || !callback_ || !pickActor_ ||
            (event->position() - pressPosition_).manhattanLength() > 4) return;
        const auto size = window_->GetSize();
        const double ratio = devicePixelRatioF();
        const double x = event->position().x() * ratio;
        const double y = size[1] - 1 - event->position().y() * ratio;
        const double diagonal = std::max(1.0, std::hypot(double(size[0]), double(size[1])));
        double point[3];
        bool picked = false;
        if (pickSurface_) {
            vtkNew<vtkCellPicker> picker;
            picker->SetTolerance(2.0 / diagonal);
            picker->PickFromListOn();
            picker->AddPickList(pickActor_);
            picked = picker->Pick(x, y, 0, renderer_) != 0 && picker->GetCellId() >= 0;
            if (picked) std::copy_n(picker->GetPickPosition(), 3, point);
        } else {
            vtkNew<vtkPointPicker> picker;
            picker->SetTolerance(5.0 / diagonal);
            picker->PickFromListOn();
            picker->AddPickList(pickActor_);
            picked = picker->Pick(x, y, 0, renderer_) != 0 && picker->GetPointId() >= 0;
            if (picked) std::copy_n(picker->GetPickPosition(), 3, point);
        }
        if (picked) {
            auto callback = std::move(callback_);
            clearArm();
            callback(QVector3D(float(point[0]), float(point[1]), float(point[2])));
        }
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape) clearArm();
        else QVTKOpenGLNativeWidget::keyPressEvent(event);
    }

private:
    bool pickSurface_ = false;
    vtkSmartPointer<vtkGenericOpenGLRenderWindow> window_;
    vtkSmartPointer<vtkRenderer> renderer_;
    vtkActor *pickActor_ = nullptr;
    std::function<void(const QVector3D &)> callback_;
    std::function<void(const QRect &)> rectangleCallback_;
    QRubberBand *rubberBand_ = nullptr;
    vtkSmartPointer<vtkCamera> rectangleCamera_;
    QPointF pressPosition_;
    bool viewInitialized_ = false;
    bool draggingRectangle_ = false;
    bool rectangleDisabledInteractor_ = false;
};

vtkSmartPointer<vtkActor> marker(const QVector3D &point, double radius, const double color[3]) {
    vtkNew<vtkSphereSource> sphere;
    sphere->SetCenter(point.x(), point.y(), point.z());
    sphere->SetRadius(radius);
    sphere->SetThetaResolution(12);
    sphere->SetPhiResolution(12);
    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputConnection(sphere->GetOutputPort());
    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetColor(color[0], color[1], color[2]);
    return actor;
}

vtkSmartPointer<vtkBillboardTextActor3D> markerLabel(const QVector3D &point, int number,
                                                     const double color[3]) {
    auto label = vtkSmartPointer<vtkBillboardTextActor3D>::New();
    label->SetInput(QString::number(number).toUtf8().constData());
    label->SetPosition(point.x(), point.y(), point.z());
    label->GetTextProperty()->SetFontSize(20);
    label->GetTextProperty()->SetColor(color[0], color[1], color[2]);
    label->GetTextProperty()->SetBold(true);
    return label;
}

double markerRadius(vtkPolyData *geometry) {
    if (!geometry) return 0.08;
    double bounds[6]; geometry->GetBounds(bounds);
    return std::clamp(std::max({bounds[1] - bounds[0], bounds[3] - bounds[2],
                                bounds[5] - bounds[4]}) * 0.003, 0.025, 0.3);
}

QString pointText(const QVector3D &point) {
    return QStringLiteral("%1, %2, %3").arg(point.x(), 0, 'f', 3)
        .arg(point.y(), 0, 'f', 3).arg(point.z(), 0, 'f', 3);
}

class RegistrationWindow : public QMainWindow {
public:
    explicit RegistrationWindow(const QString &project, const QString &facility, const QString &inspection,
                                const QString &modelPath, const QString &cloudPath, const QString &reviewId)
        : projectDb_(project), facilityId_(facility), inspectionId_(inspection) {
        setWindowTitle(QStringLiteral("InspectMap | 정합 작업기"));
        resize(1540, 900);
        auto *root = new QWidget;
        auto *layout = new QVBoxLayout(root);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *title = new QLabel(QStringLiteral("InspectMap · 정합 작업기"));
        title->setStyleSheet(QStringLiteral("background:#243d50;color:white;font-size:21px;font-weight:bold;padding:17px;"));
        layout->addWidget(title);
        auto *projectLabel = new QLabel(projectDb_.isEmpty() ? QStringLiteral("독립 실행 · 결과 저장에는 프로젝트가 필요합니다") : projectDb_);
        projectLabel->setStyleSheet(QStringLiteral("color:#526b7a;padding:6px 16px;"));
        layout->addWidget(projectLabel);

        auto *split = new QSplitter(Qt::Horizontal);
        auto *controls = new QWidget;
        auto *form = new QVBoxLayout(controls);
        form->setContentsMargins(16, 8, 16, 8);
        form->addWidget(heading(QStringLiteral("입력")));
        modelPath_ = new QLineEdit;
        modelPath_->setReadOnly(true);
        modelPath_->setPlaceholderText(QStringLiteral("모델 폴더"));
        form->addWidget(modelPath_);
        auto *chooseModel = new QPushButton(QStringLiteral("모델 폴더 선택"));
        form->addWidget(chooseModel);
        units_ = new QComboBox;
        units_->addItem(QStringLiteral("메타 없으면 mm"), 0.001);
        units_->addItem(QStringLiteral("메타 없으면 m"), 1.0);
        form->addWidget(units_);
        modelInfo_ = new QLabel;
        modelInfo_->setWordWrap(true);
        form->addWidget(modelInfo_);
        form->addWidget(heading(QStringLiteral("정합 대상 부재")));
        sideMode_ = new QComboBox;
        sideMode_->addItem(QStringLiteral("조사 모드 선택"), 0);
        sideMode_->addItem(QStringLiteral("외관 조사 · EX 면"), 1);
        sideMode_->addItem(QStringLiteral("내부 조사 · IN 면"), 2);
        form->addWidget(sideMode_);
        partList_ = new QListWidget;
        partList_->setMaximumHeight(150);
        form->addWidget(partList_);
        selectedInfo_ = new QLabel(QStringLiteral("정합할 부재를 선택하십시오."));
        selectedInfo_->setWordWrap(true);
        form->addWidget(selectedInfo_);
        cloudList_ = new QComboBox;
        cloudList_->setMinimumWidth(280);
        cloudList_->addItem(QStringLiteral("LAS 선택"));
        form->addWidget(cloudList_);
        auto *chooseCloud = new QPushButton(QStringLiteral("LAS 파일 선택"));
        form->addWidget(chooseCloud);
        cloudInfo_ = new QLabel;
        cloudInfo_->setWordWrap(true);
        form->addWidget(cloudInfo_);
        form->addSpacing(12);
        form->addWidget(heading(QStringLiteral("자동정합")));
        auto *icpModes = new QHBoxLayout;
        icpModes->addWidget(new QLabel(QStringLiteral("ICP 변환")));
        auto *icpModeGroup = new QButtonGroup(this);
        icpModeGroup->setExclusive(true);
        auto *icpRigid = new QPushButton(QStringLiteral("RigidBody"));
        icpSimilarity_ = new QPushButton(QStringLiteral("Similarity"));
        icpRigid->setToolTip(QStringLiteral("회전과 이동만 추정하며 스케일을 고정합니다."));
        icpSimilarity_->setToolTip(QStringLiteral("회전·이동과 균일 스케일을 함께 추정합니다."));
        for (auto *button : {icpRigid, icpSimilarity_}) {
            button->setCheckable(true);
            button->setStyleSheet(QStringLiteral("QPushButton {padding:6px;border:1px solid #b9cbd1;} "
                "QPushButton:checked {background:#0f766e;color:white;border-color:#0f766e;}"));
            icpModeGroup->addButton(button);
            icpModes->addWidget(button, 1);
        }
        icpRigid->setChecked(true);
        form->addLayout(icpModes);
        auto *automatic = new QPushButton(QStringLiteral("ICP 자동·정밀 정합"));
        automatic->setToolTip(QStringLiteral("원본 점군 또는 수동 정합 결과에서 0°·180°를 각각 계산합니다."));
        form->addWidget(automatic);
        orientationTable_ = new QTableWidget(0, 4);
        orientationTable_->setHorizontalHeaderLabels({QStringLiteral("초기 방향"), QStringLiteral("표면 RMSE(m)"),
            QStringLiteral("겹침률"), QStringLiteral("판정")});
        orientationTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        orientationTable_->verticalHeader()->hide();
        orientationTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        orientationTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        orientationTable_->setItemDelegate(new RowSelectionDelegate(orientationTable_));
        orientationTable_->setStyleSheet(QStringLiteral(
            "QTableWidget::item:selected {background:#dcece8;color:#173b37;border:0;border-radius:0;}"
            "QTableWidget::item:focus {outline:none;border:0;}"));
        orientationTable_->setMaximumHeight(110);
        orientationTable_->hide();
        form->addWidget(orientationTable_);
        orientationStatus_ = new QLabel;
        orientationStatus_->setWordWrap(true);
        orientationStatus_->hide();
        form->addWidget(orientationStatus_);
        metrics_ = new QLabel(QStringLiteral("정합 결과 없음"));
        metrics_->setWordWrap(true);
        form->addWidget(metrics_);
        auto *manualToggle = new QToolButton;
        manualToggle->setText(QStringLiteral("수동정합"));
        manualToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        manualToggle->setArrowType(Qt::RightArrow);
        manualToggle->setCheckable(true);
        manualToggle->setStyleSheet(QStringLiteral("QToolButton {font-size:16px;font-weight:bold;color:#233f50;border:0;padding:6px 0;}"));
        form->addWidget(manualToggle);
        auto *manualPanel = new QWidget;
        auto *manualLayout = new QVBoxLayout(manualPanel);
        manualLayout->setContentsMargins(0, 0, 0, 0);
        manualPanel->setVisible(false);
        connect(manualToggle, &QToolButton::toggled, manualPanel, [manualToggle, manualPanel](bool open) {
            manualToggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
            manualPanel->setVisible(open);
        });
        auto *pickActions = new QHBoxLayout;
        auto *pickSource = new QPushButton(QStringLiteral("점군 점 선택"));
        auto *pickTarget = new QPushButton(QStringLiteral("모델 점 선택"));
        pickActions->addWidget(pickSource);
        pickActions->addWidget(pickTarget);
        manualLayout->addLayout(pickActions);
        pairs_ = new QTableWidget(0, 3);
        pairs_->setHorizontalHeaderLabels({QStringLiteral("#"), QStringLiteral("LAS (m)"), QStringLiteral("모델 (m)")});
        pairs_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        pairs_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        pairs_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        pairs_->verticalHeader()->hide();
        pairs_->setSelectionBehavior(QAbstractItemView::SelectRows);
        pairs_->setItemDelegate(new RowSelectionDelegate(pairs_));
        pairs_->setStyleSheet(QStringLiteral(
            "QTableWidget::item:selected {background:#dcece8;color:#173b37;border:0;border-radius:0;}"
            "QTableWidget::item:focus {outline:none;border:0;}"));
        pairs_->setMinimumHeight(180);
        manualLayout->addWidget(pairs_);
        auto *editActions = new QHBoxLayout;
        auto *removePair = new QPushButton(QStringLiteral("선택 쌍 삭제"));
        auto *undoPair = new QPushButton(QStringLiteral("마지막 쌍 취소"));
        auto *clearPairs = new QPushButton(QStringLiteral("전체 지우기"));
        editActions->addWidget(removePair);
        editActions->addWidget(undoPair);
        editActions->addWidget(clearPairs);
        manualLayout->addLayout(editActions);
        allowScale_ = new QCheckBox(QStringLiteral("스케일 추정 (SfM 점군)"));
        manualLayout->addWidget(allowScale_);
        auto *manual = new QPushButton(QStringLiteral("대응점 정합"));
        manualLayout->addWidget(manual);
        form->addWidget(manualPanel);
        auto *adjustToggle = new QToolButton;
        adjustToggle->setText(QStringLiteral("결과 미세 조정"));
        adjustToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        adjustToggle->setArrowType(Qt::RightArrow);
        adjustToggle->setCheckable(true);
        adjustToggle->setStyleSheet(QStringLiteral("QToolButton {font-size:16px;font-weight:bold;color:#233f50;border:0;padding:6px 0;}"));
        form->addWidget(adjustToggle);
        auto *adjustPanel = new QWidget;
        auto *adjustLayout = new QVBoxLayout(adjustPanel);
        adjustLayout->setContentsMargins(0, 0, 0, 0);
        adjustPanel->setVisible(false);
        connect(adjustToggle, &QToolButton::toggled, adjustPanel, [adjustToggle, adjustPanel](bool open) {
            adjustToggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
            adjustPanel->setVisible(open);
        });
        auto *adjustGrid = new QGridLayout;
        const QStringList axes{QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z"),
                               QStringLiteral("Rx"), QStringLiteral("Ry"), QStringLiteral("Rz")};
        for (int i = 0; i < 6; ++i) {
            auto *value = new QDoubleSpinBox;
            value->setRange(i < 3 ? -1000.0 : -180.0, i < 3 ? 1000.0 : 180.0);
            value->setDecimals(i < 3 ? 3 : 2);
            value->setSingleStep(i < 3 ? 0.05 : 0.5);
            value->setSuffix(i < 3 ? QStringLiteral(" m") : QStringLiteral(" °"));
            adjustValues_[i] = value;
            adjustGrid->addWidget(new QLabel(axes[i]), i / 3, (i % 3) * 2);
            adjustGrid->addWidget(value, i / 3, (i % 3) * 2 + 1);
        }
        adjustLayout->addLayout(adjustGrid);
        auto *adjustActions = new QHBoxLayout;
        auto *applyAdjust = new QPushButton(QStringLiteral("조정 적용"));
        auto *undoAdjust = new QPushButton(QStringLiteral("조정 취소"));
        adjustActions->addWidget(applyAdjust);
        adjustActions->addWidget(undoAdjust);
        adjustLayout->addLayout(adjustActions);
        form->addWidget(adjustPanel);
        poseSameFrame_ = new QCheckBox(QStringLiteral("확정 시 카메라 위치·자세에도 정합 결과 적용"));
        poseSameFrame_->setToolTip(QStringLiteral("카메라 초기 pose와 LAS가 동일한 원본 좌표계일 때만 선택하십시오."));
        poseSameFrame_->setEnabled(false);
        form->addWidget(poseSameFrame_);
        save_ = new QPushButton(QStringLiteral("정합 후보 저장"));
        save_->setEnabled(false);
        save_->setStyleSheet(QStringLiteral("background:#0f766e;color:white;padding:9px;"));
        form->addWidget(save_);
        confirm_ = new QPushButton(QStringLiteral("검토 확정"));
        confirm_->setEnabled(false);
        form->addWidget(confirm_);
        auto *controlScroll = new QScrollArea;
        controlScroll->setWidgetResizable(true);
        controlScroll->setFrameShape(QFrame::NoFrame);
        controlScroll->setWidget(controls);
        split->addWidget(controlScroll);

        views_ = new QTabWidget;
        auto *pairPage = new QWidget;
        auto *pairLayout = new QHBoxLayout(pairPage);
        auto *sourceColumn = new QVBoxLayout;
        sourceColumn->addWidget(heading(QStringLiteral("LiDAR 점군")));
        auto *sourceTools = new QHBoxLayout;
        auto *fitCloud = new QPushButton(QStringLiteral("전체 보기"));
        auto *pivotCloud = new QPushButton(QStringLiteral("회전 중심"));
        auto *cropCloud = new QPushButton(QStringLiteral("영역 선택"));
        auto *resetCloud = new QPushButton(QStringLiteral("영역 해제"));
        for (auto *button : {fitCloud, pivotCloud, cropCloud, resetCloud}) sourceTools->addWidget(button);
        sourceColumn->addLayout(sourceTools);
        sourceView_ = new PickView(false);
        sourceColumn->addWidget(sourceView_, 1);
        auto *targetColumn = new QVBoxLayout;
        targetColumn->addWidget(heading(QStringLiteral("기준 모델")));
        auto *targetTools = new QHBoxLayout;
        modelMode_ = new QComboBox;
        modelMode_->addItems({QStringLiteral("선택 부재"), QStringLiteral("전체 참조"),
                              QStringLiteral("선택 와이어")});
        modelMode_->setCurrentIndex(1);
        auto *fitPart = new QPushButton(QStringLiteral("선택에 맞춤"));
        auto *pivotPart = new QPushButton(QStringLiteral("회전 중심"));
        targetTools->addWidget(modelMode_);
        targetTools->addWidget(fitPart);
        targetTools->addWidget(pivotPart);
        targetColumn->addLayout(targetTools);
        auto *axisTools = new QHBoxLayout;
        for (int axis = 0; axis < 3; ++axis) {
            auto *button = new QPushButton(QStringLiteral("%1축").arg(QLatin1Char('X' + axis)));
            axisTools->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, axis] { targetView_->axisView(axis); });
        }
        targetColumn->addLayout(axisTools);
        targetView_ = new PickView(true);
        targetColumn->addWidget(targetView_, 1);
        pairLayout->addLayout(sourceColumn, 1);
        pairLayout->addLayout(targetColumn, 1);
        views_->addTab(pairPage, QStringLiteral("대응점"));
        auto *resultPage = new QWidget;
        auto *resultLayout = new QVBoxLayout(resultPage);
        auto *cameraBar = new QHBoxLayout;
        cameraBar->addWidget(new QLabel(QStringLiteral("미리보기 좌표계")));
        auto *previewModes = new QButtonGroup(this);
        previewModes->setExclusive(true);
        previewOriginal_ = new QPushButton(QStringLiteral("원본 · LAS"));
        previewAligned_ = new QPushButton(QStringLiteral("정합 후 · 모델"));
        for (auto *button : {previewOriginal_, previewAligned_}) {
            button->setCheckable(true);
            button->setStyleSheet(QStringLiteral("QPushButton {padding:5px 10px;border:1px solid #b9cbd1;} "
                "QPushButton:checked {background:#0f766e;color:white;border-color:#0f766e;}"));
            previewModes->addButton(button);
            cameraBar->addWidget(button);
        }
        previewAligned_->setChecked(true);
        showPreviewCloud_ = new QCheckBox(QStringLiteral("점군"));
        showPreviewCloud_->setChecked(true);
        showCameraCenters_ = new QCheckBox(QStringLiteral("촬영 위치"));
        showCameraCenters_->setChecked(true);
        showSelectedFrustum_ = new QCheckBox(QStringLiteral("프러스텀"));
        showSelectedFrustum_->setChecked(true);
        previewCamera_ = new QComboBox;
        previewCamera_->setMinimumWidth(220);
        cameraStatus_ = new QLabel(QStringLiteral("연결된 카메라 pose 없음"));
        showCameraCenters_->setEnabled(false);
        showSelectedFrustum_->setEnabled(false);
        previewCamera_->setEnabled(false);
        cameraBar->addWidget(showPreviewCloud_);
        cameraBar->addWidget(showCameraCenters_);
        cameraBar->addWidget(showSelectedFrustum_);
        cameraBar->addWidget(previewCamera_);
        cameraBar->addWidget(cameraStatus_, 1);
        resultLayout->addLayout(cameraBar);
        auto *modelBar = new QHBoxLayout;
        modelBar->addWidget(new QLabel(QStringLiteral("모델 표시")));
        previewModelMode_ = new QComboBox;
        previewModelMode_->addItems({QStringLiteral("표면"), QStringLiteral("와이어프레임")});
        previewModelMode_->setToolTip(QStringLiteral("정합 후 미리보기의 전체 모델과 선택 부재 표시 방식"));
        modelBar->addWidget(previewModelMode_);
        modelBar->addStretch(1);
        resultLayout->addLayout(modelBar);
        resultView_ = new PickView(false);
        resultLayout->addWidget(resultView_, 1);
        views_->addTab(resultPage, QStringLiteral("정합 미리보기"));
        split->addWidget(views_);
        split->setSizes({440, 1100});
        layout->addWidget(split, 1);
        status_ = new QLabel(QStringLiteral("모델과 LAS를 선택하십시오."));
        status_->setStyleSheet(QStringLiteral("background:#eef3f5;color:#355466;padding:8px 16px;"));
        layout->addWidget(status_);
        setCentralWidget(root);

        connect(previewOriginal_, &QPushButton::clicked, this, [this] {
            previewModelMode_->setEnabled(false);
            updateOverlay();
        });
        connect(previewAligned_, &QPushButton::clicked, this, [this] {
            previewModelMode_->setEnabled(true);
            updateOverlay();
        });
        connect(previewModelMode_, &QComboBox::currentIndexChanged, this, [this] { updateOverlay(false); });
        connect(showPreviewCloud_, &QCheckBox::toggled, this, [this] { updateOverlay(false); });
        connect(showCameraCenters_, &QCheckBox::toggled, this, [this] { updateOverlay(false); });
        connect(showSelectedFrustum_, &QCheckBox::toggled, this, [this] { updateOverlay(false); });
        connect(previewCamera_, &QComboBox::currentIndexChanged, this, [this] { updateOverlay(false); });

        connect(chooseModel, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getExistingDirectory(this, QStringLiteral("모델 폴더"));
            if (!path.isEmpty()) loadModel(path);
        });
        connect(chooseCloud, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("LAS 파일"), {}, QStringLiteral("LAS (*.las)"));
            if (!path.isEmpty()) {
                cloudList_->addItem(QFileInfo(path).fileName(), path);
                cloudList_->setCurrentIndex(cloudList_->count() - 1);
            }
        });
        connect(cloudList_, &QComboBox::currentIndexChanged, this, [this](int index) {
            if (index > 0) loadCloud(cloudList_->itemData(index).toString());
        });
        connect(partList_, &QListWidget::itemChanged, this, [this] { updateSelectedParts(); });
        connect(sideMode_, &QComboBox::currentIndexChanged, this, [this] {
            sourcePairs_.clear(); targetPairs_.clear();
            pendingSource_.reset(); pendingTarget_.reset();
            refreshPairs();
            updateSelectedParts();
        });
        connect(modelMode_, &QComboBox::currentIndexChanged, this, [this] { updateModelView(); });
        connect(fitPart, &QPushButton::clicked, this, [this] {
            targetView_->fitGeometry(selectedModel_ ? selectedModel_.GetPointer() : model_.GetPointer());
        });
        connect(fitCloud, &QPushButton::clicked, this, [this] { sourceView_->fit(); });
        connect(pivotPart, &QPushButton::clicked, this, [this] {
            if (!selectedModel_) { status_->setText(QStringLiteral("먼저 모델 부재를 선택하십시오.")); return; }
            targetView_->arm([this](const QVector3D &point) {
                targetView_->setPivot(point);
                adjustPivot_ = point;
                status_->setText(QStringLiteral("모델 회전 중심: %1").arg(pointText(point)));
            });
            status_->setText(QStringLiteral("모델에서 회전 중심을 클릭하십시오."));
        });
        connect(pivotCloud, &QPushButton::clicked, this, [this] {
            sourceView_->arm([this](const QVector3D &point) {
                sourceView_->setPivot(point);
                status_->setText(QStringLiteral("LAS 회전 중심: %1").arg(pointText(point)));
            });
            status_->setText(QStringLiteral("점군에서 회전 중심을 클릭하십시오."));
        });
        connect(cropCloud, &QPushButton::clicked, this, [this] {
            if (!cloudGeometry_) return;
            sourceView_->armRectangle([this](const QRect &area) { cropCloudToScreen(area); });
            status_->setText(QStringLiteral("점군에서 사용할 영역을 사각형으로 드래그하십시오."));
        });
        connect(resetCloud, &QPushButton::clicked, this, [this] { resetCloudCrop(); });
        connect(pickSource, &QPushButton::clicked, this, [this] {
            views_->setCurrentIndex(0);
            sourceView_->arm([this](const QVector3D &point) { sourcePicked(point); });
            status_->setText(QStringLiteral("점군에서 대응점을 선택합니다."));
        });
        connect(pickTarget, &QPushButton::clicked, this, [this] {
            views_->setCurrentIndex(0);
            targetView_->arm([this](const QVector3D &point) { targetPicked(point); });
            status_->setText(QStringLiteral("모델 표면에서 대응점을 선택합니다."));
        });
        connect(removePair, &QPushButton::clicked, this, [this] {
            const int row = pairs_->currentRow();
            if (row < 0) return;
            sourcePairs_.removeAt(row); targetPairs_.removeAt(row);
            refreshPairs(); invalidateResult();
        });
        connect(clearPairs, &QPushButton::clicked, this, [this] {
            sourcePairs_.clear(); targetPairs_.clear();
            pendingSource_.reset(); pendingTarget_.reset();
            refreshPairs(); invalidateResult();
        });
        connect(undoPair, &QPushButton::clicked, this, [this] {
            if (pendingSource_ || pendingTarget_) {
                pendingSource_.reset(); pendingTarget_.reset();
            } else if (!sourcePairs_.isEmpty()) {
                sourcePairs_.removeLast(); targetPairs_.removeLast();
            }
            refreshPairs(); invalidateResult();
        });
        connect(manual, &QPushButton::clicked, this, [this] { runManual(); });
        connect(automatic, &QPushButton::clicked, this, [this] { runIcp(); });
        connect(orientationTable_, &QTableWidget::cellClicked, this, [this](int row) {
            if (row < 0 || row >= orientationComparison_.candidates.size()) return;
            const auto &candidate = orientationComparison_.candidates[row];
            presentResult(QStringLiteral("ICP · %1° · ").arg(candidate.angleDegrees) +
                (comparisonSimilarity_ ? QStringLiteral("Similarity · ") : QStringLiteral("RigidBody · ")) +
                sideMode_->currentText(), candidate.matrix, landmarkRmse_, candidate.iterations);
            orientationStatus_->setText(orientationComparison_.suggestedIndex < 0 ?
                QStringLiteral("방향 판별 필요 · 선택한 후보의 촬영 위치·프러스텀과 기준점을 확인하십시오.") :
                QStringLiteral("%1° 후보 추천 · 촬영 위치·프러스텀과 기준점을 확인한 뒤 저장하십시오.")
                    .arg(orientationComparison_.candidates[orientationComparison_.suggestedIndex].angleDegrees));
        });
        connect(applyAdjust, &QPushButton::clicked, this, [this] { applyAdjustment(); });
        connect(undoAdjust, &QPushButton::clicked, this, [this] { undoAdjustment(); });
        connect(save_, &QPushButton::clicked, this, [this] { saveResult(); });
        connect(confirm_, &QPushButton::clicked, this, [this] { confirmSavedResult(); });

        QString selectedModel = modelPath;
        QString selectedCloud = cloudPath;
        RegistrationReviewInfo savedReview;
        if (!projectDb_.isEmpty()) {
            QString error;
            ProjectStore::ensureManagerSchema(projectDb_, &error);
            if (!reviewId.isEmpty()) {
                QList<RegistrationReviewInfo> reviews;
                if (ProjectStore::pendingRegistrations(projectDb_, {}, &reviews, &error)) {
                    for (const auto &item : reviews)
                        if (item.id == reviewId) { savedReview = item; break; }
                    if (!savedReview.id.isEmpty()) {
                        selectedModel = savedReview.modelRoot;
                        selectedCloud = savedReview.cloudPath;
                        if (!savedReview.inspectionId.isEmpty()) inspectionId_ = savedReview.inspectionId;
                    } else status_->setText(QStringLiteral("미검토 정합 후보를 찾지 못했습니다."));
                } else status_->setText(QStringLiteral("정합 후보 조회 실패: ") + error);
            }
            if (facilityId_.isEmpty() && !inspectionId_.isEmpty()) {
                const QString connection = id();
                {
                    auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
                    db.setDatabaseName(projectDb_);
                    if (db.open()) {
                        QSqlQuery query(db);
                        query.prepare(QStringLiteral("SELECT facility_id FROM inspection WHERE inspection_id=?"));
                        query.addBindValue(inspectionId_);
                        if (query.exec() && query.next()) facilityId_ = query.value(0).toString();
                    }
                    db.close();
                }
                QSqlDatabase::removeDatabase(connection);
            }
            if (facilityId_.isEmpty()) {
                QList<FacilityInfo> facilities;
                if (ProjectStore::facilities(projectDb_, &facilities, &error) && facilities.size() == 1)
                    facilityId_ = facilities.first().id;
            }
            if (inspectionId_.isEmpty() && !facilityId_.isEmpty()) {
                QList<InspectionInfo> inspections;
                if (ProjectStore::inspections(projectDb_, facilityId_, &inspections, &error) && inspections.size() == 1)
                    inspectionId_ = inspections.first().id;
            }
            if (selectedModel.isEmpty() && !facilityId_.isEmpty())
                ProjectStore::facilityModelPath(projectDb_, facilityId_, &selectedModel, &error);
            if (selectedCloud.isEmpty() && !inspectionId_.isEmpty())
                ProjectStore::inspectionCloudPath(projectDb_, inspectionId_, &selectedCloud, &error);
            populateClouds(false);
        }
        if (!selectedModel.isEmpty()) {
            if (QFileInfo(selectedModel).isDir()) loadModel(selectedModel);
            else status_->setText(QStringLiteral("등록된 모델 폴더가 없습니다: %1").arg(selectedModel));
        }
        if (!selectedCloud.isEmpty()) {
            if (!QFileInfo(selectedCloud).isFile())
                status_->setText(QStringLiteral("등록된 LAS 파일이 없습니다: %1").arg(selectedCloud));
            else {
                int row = cloudList_->findData(selectedCloud);
                if (row < 0) {
                    cloudList_->addItem(QFileInfo(selectedCloud).fileName(), selectedCloud);
                    row = cloudList_->count() - 1;
                    cloudList_->setItemData(row, selectedCloud, Qt::ToolTipRole);
                }
                cloudList_->setCurrentIndex(row);
            }
        } else if (cloudList_->count() > 1) {
            for (int row = 1; row < cloudList_->count(); ++row)
                if (QFileInfo(cloudList_->itemData(row).toString()).isFile()) {
                    cloudList_->setCurrentIndex(row); break;
                }
        }
        if (!savedReview.id.isEmpty()) showSavedReview(savedReview);
    }

private:
    QLabel *heading(const QString &text) {
        auto *label = new QLabel(text);
        label->setStyleSheet(QStringLiteral("font-size:16px;font-weight:bold;color:#233f50;margin-top:6px;"));
        return label;
    }

    void populateClouds(bool selectDefault) {
        const QString connection = id();
        int defaultRow = -1;
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(projectDb_);
            if (db.open()) {
                QSqlQuery query(db);
                query.prepare(QStringLiteral("SELECT c.point_cloud_id,c.dataset_id,c.source_path,c.source_scope "
                    "FROM point_cloud c JOIN dataset d ON d.dataset_id=c.dataset_id "
                    "WHERE (?='' OR d.inspection_id=? OR d.inspection_id IS NULL) "
                    "ORDER BY CASE c.source_scope WHEN 'Inspection' THEN 0 "
                    "WHEN 'Flight' THEN 1 ELSE 2 END,c.source_path"));
                query.addBindValue(inspectionId_);
                query.addBindValue(inspectionId_);
                if (query.exec()) while (query.next()) {
                    const QString path = query.value(2).toString();
                    const QString label = query.value(3).toString() + QStringLiteral(" · ") + QFileInfo(path).fileName();
                    cloudList_->addItem(label, path);
                    const int row = cloudList_->count() - 1;
                    cloudList_->setItemData(row, query.value(0), Qt::UserRole + 1);
                    cloudList_->setItemData(row, query.value(1), Qt::UserRole + 2);
                    cloudList_->setItemData(row, path, Qt::ToolTipRole);
                    if (defaultRow < 0 && QFileInfo(path).isFile()) defaultRow = row;
                }
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
        if (selectDefault && defaultRow > 0) cloudList_->setCurrentIndex(defaultRow);
    }

    void loadModel(const QString &path) {
        vtkNew<vtkPolyData> geometry;
        QVector<RegistrationPart> parts;
        QString error;
        if (!RegistrationEngine::loadModel(path, units_->currentData().toDouble(), &parts, geometry, &error)) {
            QMessageBox::warning(this, QStringLiteral("모델 입력 실패"), error); return;
        }
        model_ = vtkSmartPointer<vtkPolyData>::New();
        model_->DeepCopy(geometry);
        parts_ = std::move(parts);
        modelRoot_ = QFileInfo(path).absoluteFilePath();
        modelPath_->setText(modelRoot_);
        partList_->blockSignals(true);
        partList_->clear();
        for (const auto &part : parts_) {
            auto *item = new QListWidgetItem(part.id, partList_);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Unchecked);
        }
        partList_->blockSignals(false);
        targetView_->renderer()->RemoveAllViewProps();
        targetActor_ = modelActor(model_, 0.45);
        targetActor_->GetProperty()->SetRepresentationToWireframe();
        targetActor_->GetProperty()->SetLineWidth(1.0);
        targetActor_->GetProperty()->LightingOff();
        targetActor_->PickableOff();
        targetView_->renderer()->AddActor(targetActor_);
        targetView_->setPickActor(nullptr);
        targetView_->fit();
        const QString package = QDir(path).exists(QStringLiteral("model")) ? path :
            QFileInfo(path).fileName().compare(QStringLiteral("model"), Qt::CaseInsensitive) == 0 ?
                QFileInfo(path).absoluteDir().absolutePath() : path;
        const int drawings = QDir(package).exists(QStringLiteral("map")) ?
            QDirIterator(QDir(package).filePath(QStringLiteral("map")), {QStringLiteral("*.dxf")},
                         QDir::Files, QDirIterator::Subdirectories).hasNext() : 0;
        int exteriorParts = 0, interiorParts = 0;
        for (const auto &part : parts_) {
            if (part.exteriorFaces) ++exteriorParts;
            if (part.interiorFaces) ++interiorParts;
        }
        modelInfo_->setText(QStringLiteral("부재 %1개 · 삼각형 %2개 · EX %3개 · IN %4개 · map %5")
            .arg(parts_.size()).arg(model_->GetNumberOfPolys())
            .arg(exteriorParts).arg(interiorParts)
            .arg(drawings ? QStringLiteral("있음") : QStringLiteral("없음")));
        sourcePairs_.clear(); targetPairs_.clear(); refreshPairs();
        updateSelectedParts();
        loadConfirmedPreview();
        status_->setText(QStringLiteral("기준 모델을 불러왔습니다."));
    }

    void updateSelectedParts() {
        vtkNew<vtkAppendPolyData> append;
        int count = 0, missing = 0;
        for (int i = 0; i < partList_->count(); ++i) {
            if (partList_->item(i)->checkState() != Qt::Checked) continue;
            const int side = sideMode_->currentData().toInt();
            auto geometry = side == 1 ? parts_[i].exteriorFaces :
                side == 2 ? parts_[i].interiorFaces : parts_[i].geometry;
            if (!geometry) { ++missing; continue; }
            append->AddInputData(geometry);
            ++count;
        }
        missingSideParts_ = missing;
        selectedModel_ = nullptr;
        if (count) {
            append->Update();
            selectedModel_ = vtkSmartPointer<vtkPolyData>::New();
            selectedModel_->DeepCopy(append->GetOutput());
            double bounds[6]; selectedModel_->GetBounds(bounds);
            adjustPivot_ = QVector3D(float((bounds[0] + bounds[1]) * 0.5),
                                     float((bounds[2] + bounds[3]) * 0.5),
                                     float((bounds[4] + bounds[5]) * 0.5));
        }
        selectedInfo_->setText(QStringLiteral("사용 가능 %1 / %2개 · %3")
            .arg(count).arg(parts_.size()).arg(missing ?
                QStringLiteral("%1개 부재에 해당 면 정보 없음").arg(missing) :
                QStringLiteral("모델 점 선택은 표시된 면만")));
        if (selectedActor_) targetView_->renderer()->RemoveActor(selectedActor_);
        selectedActor_ = nullptr;
        if (selectedModel_) {
            selectedActor_ = modelActor(selectedModel_, 0.65);
            selectedActor_->GetProperty()->SetColor(0.16, 0.82, 0.67);
            selectedActor_->GetProperty()->EdgeVisibilityOn();
            selectedActor_->GetProperty()->SetEdgeColor(0.05, 0.33, 0.27);
            targetView_->renderer()->AddActor(selectedActor_);
        }
        targetView_->setPickActor(selectedActor_);
        updateModelView();
        invalidateResult();
        updateOverlay(false);
    }

    void updateModelView() {
        if (!targetActor_) return;
        const int mode = modelMode_->currentIndex();
        targetActor_->SetVisibility(!selectedActor_ || mode == 1);
        if (selectedActor_) {
            auto *property = selectedActor_->GetProperty();
            if (mode == 2) {
                property->SetRepresentationToWireframe();
                property->SetOpacity(1.0);
                property->SetLineWidth(2.0);
            } else {
                property->SetRepresentationToSurface();
                property->SetOpacity(0.65);
                property->SetLineWidth(1.0);
            }
        }
        targetView_->renderer()->ResetCameraClippingRange();
        targetView_->refresh();
    }

    void loadCloud(const QString &path) {
        if (!QFileInfo(path).isFile()) {
            QMessageBox::warning(this, QStringLiteral("LAS 입력 실패"), path); return;
        }
        QApplication::setOverrideCursor(Qt::WaitCursor);
        CloudPreview cloud = readLasPreview(path, 120000);
        QApplication::restoreOverrideCursor();
        if (!cloud.error.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("LAS 입력 실패"), cloud.error); return;
        }
        cloud_ = std::move(cloud);
        activeCloud_ = cloud_;
        cloudPath_ = QFileInfo(path).absoluteFilePath();
        allCloudGeometry_ = RegistrationEngine::cloudGeometry(cloud_.points);
        cloudGeometry_ = allCloudGeometry_;
        sourceView_->renderer()->RemoveAllViewProps();
        sourceActor_ = cloudActor(cloudGeometry_, activeCloud_);
        sourceView_->renderer()->AddActor(sourceActor_);
        sourceView_->setPickActor(sourceActor_);
        sourceView_->fit();
        showCloudInfo();
        resolveCloudDataset();
        reloadCameraPreview();
        sourcePairs_.clear(); targetPairs_.clear(); refreshPairs();
        invalidateResult(); updateOverlay();
        loadConfirmedPreview();
        if (!savedMatrix_) status_->setText(QStringLiteral("LAS를 불러왔습니다. 대응점 또는 ICP로 정합하십시오."));
    }

    void showCloudInfo() {
        cloudInfo_->setText(QStringLiteral("%1\n전체 %2점 · 미리보기 %3점 · 작업 영역 %4점")
            .arg(cloudPath_).arg(cloud_.totalPoints).arg(cloud_.points.size())
            .arg(activeCloud_.points.size()));
    }

    void resolveCloudDataset() {
        const int row = cloudList_->currentIndex();
        if (projectDb_.isEmpty() || row < 1 ||
            !cloudList_->itemData(row, Qt::UserRole + 2).toString().isEmpty()) return;
        const QString selectedPath = QDir::cleanPath(QFileInfo(cloudPath_).absoluteFilePath());
        const QString connection = id();
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(projectDb_);
            if (db.open()) {
                QSqlQuery query(db);
                if (query.exec(QStringLiteral("SELECT point_cloud_id,dataset_id,source_path FROM point_cloud"))) {
                    while (query.next()) {
                        const QString registeredPath = QDir::cleanPath(QFileInfo(query.value(2).toString()).absoluteFilePath());
                        if (selectedPath.compare(registeredPath, Qt::CaseInsensitive) != 0) continue;
                        cloudList_->setItemData(row, query.value(0), Qt::UserRole + 1);
                        cloudList_->setItemData(row, query.value(1), Qt::UserRole + 2);
                        break;
                    }
                }
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }

    void reloadCameraPreview() {
        cameras_.clear();
        previewIntrinsics_ = {};
        {
            const QSignalBlocker blocker(previewCamera_);
            previewCamera_->clear();
        }
        const QString datasetId = cloudList_->currentData(Qt::UserRole + 2).toString();
        if (!projectDb_.isEmpty() && !datasetId.isEmpty()) {
            const QString connection = id();
            {
                auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
                db.setDatabaseName(projectDb_);
                if (db.open()) {
                    QSqlQuery query(db);
                    query.prepare(QStringLiteral("SELECT COALESCE(NULLIF(i.display_name,''),i.source_identifier),"
                        "i.width,i.height,p.pose_json FROM image i JOIN camera_pose p ON p.image_id=i.image_id "
                        "WHERE i.dataset_id=? AND p.pose_stage='INITIAL' "
                        "ORDER BY i.capture_time,i.display_name,i.image_id"));
                    query.addBindValue(datasetId);
                    if (query.exec()) while (query.next()) {
                        CameraPreviewItem item;
                        item.name = query.value(0).toString();
                        item.width = query.value(1).toInt();
                        item.height = query.value(2).toInt();
                        item.pose = parsePreviewCamera(query.value(3).toString());
                        if (item.pose.valid) cameras_.append(item);
                    }
                    db.close();
                }
            }
            QSqlDatabase::removeDatabase(connection);
            QString profilePath, error;
            if (ConversionCore::activeProfile(projectDb_, datasetId, &profilePath, &error))
                previewIntrinsics_ = readPreviewIntrinsics(profilePath);
        }
        {
            const QSignalBlocker blocker(previewCamera_);
            for (const auto &item : cameras_) previewCamera_->addItem(item.name);
        }
        const bool hasCameras = !cameras_.isEmpty();
        previewCamera_->setEnabled(hasCameras);
        showCameraCenters_->setEnabled(hasCameras);
        showSelectedFrustum_->setEnabled(hasCameras);
        poseSameFrame_->setEnabled(hasCameras);
        poseSameFrame_->setChecked(hasCameras);
        cameraStatus_->setText(hasCameras ?
            QStringLiteral("초기 pose %1개 · 프러스텀 %2 · 저장 전 미리보기")
                .arg(cameras_.size()).arg(previewIntrinsics_.valid ? QStringLiteral("보정값 적용") : QStringLiteral("근사")) :
            datasetId.isEmpty() ? QStringLiteral("LAS가 이미지 데이터 세트에 연결되지 않음") :
                QStringLiteral("연결된 데이터 세트에 초기 pose 없음"));
    }

    void useCloudSelection(CloudPreview selected) {
        activeCloud_ = std::move(selected);
        cloudGeometry_ = activeCloud_.points.size() == cloud_.points.size() ?
            allCloudGeometry_ : RegistrationEngine::cloudGeometry(activeCloud_.points);
        if (sourceActor_) sourceView_->renderer()->RemoveActor(sourceActor_);
        sourceActor_ = cloudActor(cloudGeometry_, activeCloud_);
        sourceView_->renderer()->AddActor(sourceActor_);
        sourceView_->setPickActor(sourceActor_);
        sourceView_->fitGeometry(cloudGeometry_);
        showCloudInfo();
        invalidateResult();
        updateOverlay();
    }

    void cropCloudToScreen(const QRect &area) {
        CloudPreview selected;
        selected.totalPoints = cloud_.totalPoints;
        selected.intensityLow = cloud_.intensityLow;
        selected.intensityHigh = cloud_.intensityHigh;
        for (int i = 0; i < activeCloud_.points.size(); ++i) {
            const auto projected = sourceView_->displayPoint(activeCloud_.points[i]);
            if (!projected || !area.contains(projected->toPoint())) continue;
            selected.points.append(activeCloud_.points[i]);
            selected.intensities.append(activeCloud_.intensities[i]);
        }
        if (selected.points.size() < 3) {
            status_->setText(QStringLiteral("선택 범위에 점이 3개 미만입니다. 더 넓게 선택하십시오."));
            return;
        }
        const int count = selected.points.size();
        useCloudSelection(std::move(selected));
        status_->setText(QStringLiteral("LAS 작업 영역 %1점 선택 · 원본 LAS는 변경되지 않음").arg(count));
    }

    void resetCloudCrop() {
        if (!allCloudGeometry_) return;
        useCloudSelection(cloud_);
        status_->setText(QStringLiteral("LAS 작업 영역을 전체로 되돌렸습니다."));
    }

    void sourcePicked(const QVector3D &point) {
        pendingSource_ = point;
        completePair();
    }

    void targetPicked(const QVector3D &point) {
        pendingTarget_ = point;
        completePair();
    }

    void completePair() {
        if (pendingSource_ && pendingTarget_) {
            sourcePairs_.append(*pendingSource_);
            targetPairs_.append(*pendingTarget_);
            pendingSource_.reset(); pendingTarget_.reset();
            refreshPairs(); invalidateResult();
            status_->setText(QStringLiteral("대응점 %1쌍 지정됨").arg(sourcePairs_.size()));
        } else {
            refreshPairs();
            status_->setText(pendingSource_ ? QStringLiteral("같은 위치의 모델 점을 선택합니다.") :
                                            QStringLiteral("같은 위치의 점군 점을 선택합니다."));
        }
    }

    void refreshPairs() {
        pairs_->setRowCount(sourcePairs_.size());
        for (int row = 0; row < sourcePairs_.size(); ++row) {
            pairs_->setItem(row, 0, new QTableWidgetItem(QString::number(row + 1)));
            pairs_->setItem(row, 1, new QTableWidgetItem(pointText(sourcePairs_[row])));
            pairs_->setItem(row, 2, new QTableWidgetItem(pointText(targetPairs_[row])));
        }
        for (const auto &actor : sourceMarkers_) sourceView_->renderer()->RemoveActor(actor);
        for (const auto &actor : targetMarkers_) targetView_->renderer()->RemoveActor(actor);
        for (const auto &label : sourceLabels_) sourceView_->renderer()->RemoveViewProp(label);
        for (const auto &label : targetLabels_) targetView_->renderer()->RemoveViewProp(label);
        if (pendingSourceMarker_) sourceView_->renderer()->RemoveActor(pendingSourceMarker_);
        if (pendingTargetMarker_) targetView_->renderer()->RemoveActor(pendingTargetMarker_);
        sourceMarkers_.clear(); targetMarkers_.clear();
        sourceLabels_.clear(); targetLabels_.clear();
        pendingSourceMarker_ = nullptr; pendingTargetMarker_ = nullptr;
        const double sourceColor[]{0.98, 0.23, 0.17};
        const double targetColor[]{0.99, 0.73, 0.23};
        for (int row = 0; row < sourcePairs_.size(); ++row) {
            auto source = marker(sourcePairs_[row], markerRadius(cloudGeometry_), sourceColor);
            auto target = marker(targetPairs_[row], markerRadius(selectedModel_), targetColor);
            auto sourceLabel = markerLabel(sourcePairs_[row], row + 1, sourceColor);
            auto targetLabel = markerLabel(targetPairs_[row], row + 1, targetColor);
            sourceView_->renderer()->AddActor(source);
            targetView_->renderer()->AddActor(target);
            sourceView_->renderer()->AddActor(sourceLabel);
            targetView_->renderer()->AddActor(targetLabel);
            sourceMarkers_.append(source); targetMarkers_.append(target);
            sourceLabels_.append(sourceLabel); targetLabels_.append(targetLabel);
        }
        const double pendingColor[]{1.0, 1.0, 1.0};
        if (pendingSource_) {
            pendingSourceMarker_ = marker(*pendingSource_, markerRadius(cloudGeometry_), pendingColor);
            sourceView_->renderer()->AddActor(pendingSourceMarker_);
        }
        if (pendingTarget_) {
            pendingTargetMarker_ = marker(*pendingTarget_, markerRadius(selectedModel_), pendingColor);
            targetView_->renderer()->AddActor(pendingTargetMarker_);
        }
        sourceView_->refresh(); targetView_->refresh();
    }

    void invalidateResult() {
        matrix_ = nullptr;
        icpStart_ = nullptr;
        previousMatrix_ = nullptr;
        orientationComparison_ = {};
        orientationTable_->clearContents();
        orientationTable_->setRowCount(0);
        orientationTable_->hide();
        orientationStatus_->hide();
        method_.clear();
        if (!savedMatrix_ || reviewId_.isEmpty()) reviewId_.clear();
        if (!savedMatrix_) metrics_->setText(QStringLiteral("정합 결과 없음"));
        save_->setEnabled(false);
        confirm_->setEnabled(!reviewId_.isEmpty());
        updateOverlay(false);
    }

    static vtkSmartPointer<vtkMatrix4x4> matrixFromJson(const QString &json) {
        const auto rows = QJsonDocument::fromJson(json.toUtf8()).array();
        if (rows.size() != 4) return nullptr;
        auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
        for (int row = 0; row < 4; ++row) {
            const auto columns = rows[row].toArray();
            if (columns.size() != 4) return nullptr;
            for (int column = 0; column < 4; ++column)
                matrix->SetElement(row, column, columns[column].toDouble());
        }
        return matrix;
    }

    void loadConfirmedPreview() {
        savedMatrix_ = nullptr;
        savedPoseSameFrame_ = false;
        if (projectDb_.isEmpty() || modelRoot_.isEmpty() || cloudPath_.isEmpty()) {
            metrics_->setText(QStringLiteral("정합 결과 없음"));
            return;
        }
        const QString connection = id();
        QString matrixJson, method;
        double rmse = 0, overlap = 0;
        bool poseFrame = false;
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(projectDb_);
            if (db.open() && db.tables().contains(QStringLiteral("registration_result"))) {
                QSqlQuery query(db);
                query.prepare(QStringLiteral("SELECT source_cloud_path,model_root,matrix_json,method,"
                    "surface_rmse,overlap,pose_frame_confirmed FROM registration_result "
                    "WHERE active=1 AND review_status='CONFIRMED' AND (?='' OR inspection_id=?) "
                    "ORDER BY confirmed_at DESC LIMIT 20"));
                query.addBindValue(inspectionId_); query.addBindValue(inspectionId_);
                if (query.exec()) while (query.next()) {
                    const auto samePath = [](const QString &left, const QString &right) {
                        return QDir::cleanPath(QFileInfo(left).absoluteFilePath()).compare(
                            QDir::cleanPath(QFileInfo(right).absoluteFilePath()), Qt::CaseInsensitive) == 0;
                    };
                    if (!samePath(query.value(0).toString(), cloudPath_) ||
                        !samePath(query.value(1).toString(), modelRoot_)) continue;
                    matrixJson = query.value(2).toString(); method = query.value(3).toString();
                    rmse = query.value(4).toDouble(); overlap = query.value(5).toDouble();
                    poseFrame = query.value(6).toBool();
                    break;
                }
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
        savedMatrix_ = matrixFromJson(matrixJson);
        if (!savedMatrix_) {
            metrics_->setText(QStringLiteral("정합 결과 없음"));
            updateOverlay(false);
            return;
        }
        savedPoseSameFrame_ = poseFrame;
        matrix_ = nullptr;
        reviewId_.clear();
        save_->setEnabled(false); confirm_->setEnabled(false);
        poseSameFrame_->setChecked(poseFrame);
        metrics_->setText(QStringLiteral("확정된 정합 · %1 · 표면 RMSE %2 m · 0.5 m 이내 %3%")
            .arg(method).arg(rmse, 0, 'f', 3).arg(overlap * 100, 0, 'f', 1));
        status_->setText(QStringLiteral("확정된 정합을 불러왔습니다. 부재 표시는 정합 행렬을 변경하지 않습니다."));
        updateOverlay();
        views_->setCurrentIndex(1);
    }

    void updateOverlay(bool refit = true) {
        resultView_->renderer()->RemoveAllViewProps();
        overlayCloud_ = nullptr;
        const bool aligned = previewAligned_->isChecked();
        vtkMatrix4x4 *previewMatrix = matrix_ ? matrix_.GetPointer() : savedMatrix_.GetPointer();
        const bool showAlignedPoses = matrix_ ? poseSameFrame_->isChecked() : savedPoseSameFrame_;
        const bool wireframe = previewModelMode_->currentIndex() == 1;
        if (aligned && model_) {
            auto context = modelActor(model_, wireframe ? 0.45 : 0.2);
            if (wireframe) {
                context->GetProperty()->SetRepresentationToWireframe();
                context->GetProperty()->SetLineWidth(1.0);
            }
            resultView_->renderer()->AddActor(context);
        }
        if (aligned && selectedModel_) {
            auto selected = modelActor(selectedModel_, wireframe ? 1.0 : 0.55);
            selected->GetProperty()->SetColor(0.16, 0.82, 0.67);
            if (wireframe) {
                selected->GetProperty()->SetRepresentationToWireframe();
                selected->GetProperty()->SetLineWidth(2.0);
            }
            resultView_->renderer()->AddActor(selected);
        }
        if (cloudGeometry_ && showPreviewCloud_->isChecked()) {
            overlayCloud_ = cloudActor(cloudGeometry_, activeCloud_);
            if (aligned && previewMatrix) overlayCloud_->SetUserMatrix(previewMatrix);
            resultView_->renderer()->AddActor(overlayCloud_);
        }
        if (refit && (cloudGeometry_ || (aligned && model_))) resultView_->fit();
        if ((!aligned || (previewMatrix && showAlignedPoses)) && !cameras_.isEmpty()) {
            QVector<QVector3D> centers;
            centers.reserve(cameras_.size());
            for (const auto &item : cameras_)
                centers.append(aligned ? RegistrationEngine::transformPoint(previewMatrix, item.pose.position) : item.pose.position);
            if (showCameraCenters_->isChecked())
                resultView_->renderer()->AddActor(cameraCentersActor(centers));
            const int selected = previewCamera_->currentIndex();
            if (showSelectedFrustum_->isChecked() && selected >= 0 && selected < cameras_.size()) {
                const auto &item = cameras_[selected];
                const auto position = centers[selected];
                const auto orientation = aligned ?
                    RegistrationEngine::transformOrientation(previewMatrix, item.pose.orientation) : item.pose.orientation;
                const double cameraColor[3]{1.0, 0.74, 0.22};
                auto selectedMarker = marker(position, markerRadius(selectedModel_ ? selectedModel_.GetPointer() : model_.GetPointer()) * 1.5,
                                             cameraColor);
                selectedMarker->PickableOff();
                resultView_->renderer()->AddActor(selectedMarker);
                resultView_->renderer()->AddActor(cameraFrustumActor(position, orientation, previewIntrinsics_,
                    item.width, item.height, 0.8));
            }
        }
        resultView_->renderer()->ResetCameraClippingRange();
        resultView_->refresh();
    }

    void presentResult(const QString &method, vtkMatrix4x4 *matrix, double landmarkRmse,
                       int iterations) {
        reviewId_.clear();
        confirm_->setEnabled(false);
        poseSameFrame_->setEnabled(!cameras_.isEmpty());
        matrix_ = vtkSmartPointer<vtkMatrix4x4>::New();
        matrix_->DeepCopy(matrix);
        method_ = method;
        landmarkRmse_ = sourcePairs_.size() >= 3 ? pairRmse(matrix_) : landmarkRmse;
        iterations_ = iterations;
        resultMetrics_ = RegistrationEngine::measure(cloudGeometry_, selectedModel_ ? selectedModel_.GetPointer() : model_.GetPointer(), matrix_);
        const QString pairError = sourcePairs_.size() >= 3 ?
            QString::number(landmarkRmse_, 'f', 3) + QStringLiteral(" m") : QStringLiteral("—");
        metrics_->setText(QStringLiteral("%1 · 대응점 RMSE %2 · 표면 RMSE %3 m · 0.5 m 이내 %4% · 스케일 %5 · ICP %6회")
            .arg(method).arg(pairError)
            .arg(resultMetrics_.rmse, 0, 'f', 3)
            .arg(resultMetrics_.overlap * 100, 0, 'f', 1)
            .arg(RegistrationEngine::uniformScale(matrix_), 0, 'f', 5).arg(iterations_));
        save_->setEnabled(!projectDb_.isEmpty());
        updateOverlay();
        views_->setCurrentIndex(1);
        status_->setText(resultMetrics_.overlap < 0.2 ?
            QStringLiteral("겹침이 낮습니다. 기준점·축·점군 범위를 검토하십시오.") :
            QStringLiteral("결과를 검토한 뒤 후보로 저장할 수 있습니다."));
    }

    void showSavedReview(const RegistrationReviewInfo &review) {
        if (!model_ || !cloudGeometry_) return;
        auto loaded = matrixFromJson(review.matrixJson);
        if (!loaded) return;
        matrix_ = loaded;
        savedMatrix_ = loaded;
        savedPoseSameFrame_ = review.transformPoses;
        icpStart_ = nullptr;
        method_ = review.method;
        reviewId_ = review.id;
        poseSameFrame_->setChecked(review.transformPoses);
        poseSameFrame_->setEnabled(false);
        save_->setEnabled(false);
        confirm_->setEnabled(true);
        metrics_->setText(QStringLiteral("저장된 후보 · %1 · 표면 RMSE %2 m · 0.5 m 이내 %3%")
            .arg(review.method).arg(review.surfaceRmse, 0, 'f', 3).arg(review.overlap * 100, 0, 'f', 1));
        updateOverlay();
        views_->setCurrentIndex(1);
        status_->setText(QStringLiteral("미검토 정합 후보입니다. 미리보기 확인 후 확정하십시오."));
    }

    void runManual() {
        if (!selectedModel_ || !cloudGeometry_) {
            QMessageBox::information(this, QStringLiteral("입력 필요"),
                QStringLiteral("LAS와 정합할 모델 부재를 먼저 선택하십시오."));
            return;
        }
        vtkNew<vtkMatrix4x4> fit;
        double rmse = 0;
        QString error;
        const QString runId = startRegistrationRun();
        if (!RegistrationEngine::landmarks(sourcePairs_, targetPairs_, allowScale_->isChecked(),
                                            fit, &rmse, &error)) {
            finishRegistrationRun(runId, QStringLiteral("FAILED"), error);
            QMessageBox::information(this, QStringLiteral("대응점 확인"), error); return;
        }
        icpStart_ = vtkSmartPointer<vtkMatrix4x4>::New();
        icpStart_->DeepCopy(fit);
        orientationComparison_ = {};
        orientationTable_->hide(); orientationStatus_->hide();
        presentResult(QStringLiteral("수동"), fit, rmse, 0);
        finishRegistrationRun(runId, QStringLiteral("REVIEW"), {});
    }

    void runIcp() {
        if (!model_ || !cloudGeometry_) {
            QMessageBox::information(this, QStringLiteral("입력 필요"), QStringLiteral("모델과 LAS를 먼저 선택하십시오."));
            return;
        }
        if (!selectedModel_) {
            QMessageBox::information(this, QStringLiteral("부재 선택 필요"),
                QStringLiteral("자동 정합에 사용할 모델 부재를 하나 이상 선택하십시오."));
            return;
        }
        if (sideMode_->currentData().toInt() == 0) {
            QMessageBox::information(this, QStringLiteral("조사 모드 필요"),
                QStringLiteral("외관 조사 또는 내부 조사를 선택하십시오."));
            return;
        }
        if (missingSideParts_ > 0) {
            QMessageBox::information(this, QStringLiteral("모델 면 정보 부족"),
                QStringLiteral("선택 부재 중 %1개에 해당 조사면의 OBJ 그룹(-EX-/-IN-)이 없습니다.")
                    .arg(missingSideParts_));
            return;
        }
        const QString runId = startRegistrationRun();
        QApplication::setOverrideCursor(Qt::WaitCursor);
        OrientationComparison comparison;
        QString error;
        const bool similarity = icpSimilarity_->isChecked();
        const bool ok = RegistrationEngine::compareOrientations(cloudGeometry_, selectedModel_, icpStart_,
                                                                 similarity, &comparison, &error);
        QApplication::restoreOverrideCursor();
        if (!ok) {
            finishRegistrationRun(runId, QStringLiteral("FAILED"), error);
            QMessageBox::warning(this, QStringLiteral("ICP 실패"), error); return;
        }
        orientationComparison_ = comparison;
        comparisonSimilarity_ = similarity;
        orientationTable_->setRowCount(comparison.candidates.size());
        for (int row = 0; row < comparison.candidates.size(); ++row) {
            const auto &candidate = comparison.candidates[row];
            const QStringList values{QStringLiteral("%1°").arg(candidate.angleDegrees),
                QString::number(candidate.metrics.rmse, 'f', 3),
                QStringLiteral("%1%").arg(candidate.metrics.overlap * 100, 0, 'f', 1),
                row == comparison.suggestedIndex ? QStringLiteral("추천") :
                    comparison.suggestedIndex < 0 ? QStringLiteral("방향 판별 필요") : QStringLiteral("비교")};
            for (int col = 0; col < values.size(); ++col)
                orientationTable_->setItem(row, col, new QTableWidgetItem(values[col]));
        }
        orientationTable_->show(); orientationStatus_->show();
        orientationStatus_->setText(comparison.suggestedIndex < 0 ?
            QStringLiteral("두 방향의 점군 일치도가 비슷합니다. 방향 판별 필요 · 후보를 선택해 촬영 위치·프러스텀과 기준점을 확인하십시오.") :
            QStringLiteral("%1° 후보 추천 · 자동 확정하지 않습니다. 후보를 선택해 촬영 위치·프러스텀을 확인하십시오.")
                .arg(comparison.candidates[comparison.suggestedIndex].angleDegrees));
        const auto &preview = comparison.candidates[0];
        presentResult(QStringLiteral("ICP · 0° 미리보기"), preview.matrix, landmarkRmse_, preview.iterations);
        save_->setEnabled(false);
        confirm_->setEnabled(false);
        status_->setText(QStringLiteral("0°·180° 비교 완료 · 후보를 클릭해 방향을 선택하십시오."));
        finishRegistrationRun(runId, QStringLiteral("REVIEW"), orientationStatus_->text());
    }

    QString startRegistrationRun() {
        if (projectDb_.isEmpty()) return {};
        const QString runId = id();
        QString error;
        if (!ProjectStore::recordRun(projectDb_, {runId, inspectionId_, QStringLiteral("registration"),
            QStringLiteral("RUNNING"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs), {}, {}}, &error)) {
            QMessageBox::warning(this, QStringLiteral("작업 이력 저장 실패"), error);
            return {};
        }
        return runId;
    }

    void finishRegistrationRun(const QString &runId, const QString &runStatus, const QString &message) {
        if (runId.isEmpty()) return;
        QString error;
        if (!ProjectStore::finishRun(projectDb_, runId, runStatus, message, &error))
            QMessageBox::warning(this, QStringLiteral("작업 현황 갱신 실패"), error);
    }

    double pairRmse(vtkMatrix4x4 *matrix) const {
        double squared = 0;
        for (int i = 0; i < sourcePairs_.size(); ++i) {
            const auto delta = RegistrationEngine::transformPoint(matrix, sourcePairs_[i]) - targetPairs_[i];
            squared += delta.lengthSquared();
        }
        return sourcePairs_.isEmpty() ? 0.0 : std::sqrt(squared / sourcePairs_.size());
    }

    void applyAdjustment() {
        if (!matrix_) {
            status_->setText(QStringLiteral("대응점 정합 결과를 먼저 계산하십시오."));
            return;
        }
        bool changed = false;
        for (auto *value : adjustValues_) changed |= value->value() != 0.0;
        if (!changed) return;
        previousMatrix_ = vtkSmartPointer<vtkMatrix4x4>::New();
        previousMatrix_->DeepCopy(matrix_);
        QMatrix4x4 adjustment;
        adjustment.translate(float(adjustValues_[0]->value()), float(adjustValues_[1]->value()),
                             float(adjustValues_[2]->value()));
        adjustment.translate(adjustPivot_);
        adjustment.rotate(float(adjustValues_[3]->value()), 1, 0, 0);
        adjustment.rotate(float(adjustValues_[4]->value()), 0, 1, 0);
        adjustment.rotate(float(adjustValues_[5]->value()), 0, 0, 1);
        adjustment.translate(-adjustPivot_);
        vtkNew<vtkMatrix4x4> delta;
        for (int row = 0; row < 4; ++row)
            for (int column = 0; column < 4; ++column)
                delta->SetElement(row, column, adjustment(row, column));
        vtkNew<vtkMatrix4x4> fitted;
        vtkMatrix4x4::Multiply4x4(delta, matrix_, fitted);
        for (auto *value : adjustValues_) value->setValue(0);
        presentResult(QStringLiteral("수동 미세 조정"), fitted, landmarkRmse_, 0);
    }

    void undoAdjustment() {
        if (!previousMatrix_) return;
        vtkNew<vtkMatrix4x4> prior;
        prior->DeepCopy(previousMatrix_);
        previousMatrix_ = nullptr;
        presentResult(QStringLiteral("수동"), prior, landmarkRmse_, 0);
    }

    void saveResult() {
        if (!matrix_ || projectDb_.isEmpty()) return;
        const bool hasDataset = !cloudList_->currentData(Qt::UserRole + 2).toString().isEmpty();
        if (poseSameFrame_->isChecked() && hasDataset &&
            QMessageBox::question(this, QStringLiteral("카메라 위치·자세 변환 확인"),
                QStringLiteral("카메라 초기 pose와 LAS가 동일한 원본 좌표계입니까?\n"
                               "예를 선택하면 검토 확정 시 카메라 pose에도 정합 행렬을 적용합니다.")) != QMessageBox::Yes)
            return;
        if (poseSameFrame_->isChecked() && !hasDataset &&
            QMessageBox::question(this, QStringLiteral("카메라 자세 갱신 불가"),
                QStringLiteral("이 LAS는 이미지 데이터 세트에 연결되지 않았습니다. 카메라 pose 변환 없이 후보만 저장하시겠습니까?")) != QMessageBox::Yes)
            return;
        if (resultMetrics_.overlap < 0.2 &&
            QMessageBox::question(this, QStringLiteral("정합 결과 확인"),
                QStringLiteral("0.5 m 이내 점 비율이 20% 미만입니다. 그래도 저장하시겠습니까?")) != QMessageBox::Yes)
            return;
        QString error;
        if (!ProjectStore::ensureRegistrationSchema(projectDb_, &error)) {
            QMessageBox::warning(this, QStringLiteral("정합 결과 저장 실패"), error);
            return;
        }
        const QString connection = id();
        bool saved = false;
        QString savedId;
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(projectDb_);
            if (!db.open()) error = db.lastError().text();
            else if (!db.transaction()) error = db.lastError().text();
            else {
                    const QString resultId = id();
                    const QString runId = id();
                    const int row = cloudList_->currentIndex();
                    const QString datasetId = cloudList_->itemData(row, Qt::UserRole + 2).toString();
                    const QString cloudId = cloudList_->itemData(row, Qt::UserRole + 1).toString();
                    QJsonArray rows;
                    for (int r = 0; r < 4; ++r) {
                        QJsonArray columns;
                        for (int c = 0; c < 4; ++c) columns.append(matrix_->GetElement(r, c));
                        rows.append(columns);
                    }
                    QSqlQuery insert(db);
                    insert.prepare(QStringLiteral("INSERT INTO registration_result (registration_id,inspection_id,dataset_id,point_cloud_id,"
                        "source_cloud_path,model_root,method,matrix_json,scale,landmark_rmse,surface_rmse,overlap,"
                        "icp_iterations,pose_frame_confirmed,created_at,active,survey_side,review_status,run_id) "
                        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
                    insert.addBindValue(resultId);
                    insert.addBindValue(inspectionId_.isEmpty() ? QVariant() : QVariant(inspectionId_));
                    insert.addBindValue(datasetId);
                    insert.addBindValue(cloudId);
                    insert.addBindValue(cloudPath_);
                    insert.addBindValue(modelRoot_);
                    insert.addBindValue(method_);
                    insert.addBindValue(QString::fromUtf8(QJsonDocument(rows).toJson(QJsonDocument::Compact)));
                    insert.addBindValue(RegistrationEngine::uniformScale(matrix_));
                    insert.addBindValue(landmarkRmse_);
                    insert.addBindValue(resultMetrics_.rmse);
                    insert.addBindValue(resultMetrics_.overlap);
                    insert.addBindValue(iterations_);
                    insert.addBindValue(poseSameFrame_->isChecked() && hasDataset ? 1 : 0);
                    insert.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
                    insert.addBindValue(0);
                    insert.addBindValue(sideMode_->currentData().toInt() == 1 ? QStringLiteral("EXTERIOR") :
                                        sideMode_->currentData().toInt() == 2 ? QStringLiteral("INTERIOR") : QString());
                    insert.addBindValue(QStringLiteral("PENDING"));
                    insert.addBindValue(runId);
                    bool ok = insert.exec();
                    QSqlQuery run(db);
                    if (ok) {
                        run.prepare(QStringLiteral("INSERT INTO module_run VALUES(?,?,?,?,?,?,?)"));
                        run.addBindValue(runId);
                        run.addBindValue(inspectionId_.isEmpty() ? QVariant() : QVariant(inspectionId_));
                        run.addBindValue(QStringLiteral("registration"));
                        run.addBindValue(QStringLiteral("REVIEW"));
                        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
                        run.addBindValue(now);
                        run.addBindValue(now);
                        run.addBindValue(QStringLiteral(""));
                        ok = run.exec();
                    }
                    if (ok && db.commit()) saved = true;
                    else {
                        if (error.isEmpty()) error = ok ? db.lastError().text() :
                            run.lastError().isValid() ? run.lastError().text() : insert.lastError().text();
                        db.rollback();
                    }
                    if (saved) savedId = resultId;
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
        if (saved) {
            reviewId_ = savedId;
            savedMatrix_ = vtkSmartPointer<vtkMatrix4x4>::New();
            savedMatrix_->DeepCopy(matrix_);
            savedPoseSameFrame_ = poseSameFrame_->isChecked() && hasDataset;
            save_->setEnabled(false);
            confirm_->setEnabled(true);
            poseSameFrame_->setEnabled(false);
            status_->setText(QStringLiteral("정합 후보 저장 완료 · 검토 확정 전에는 활성 결과와 카메라 pose가 변경되지 않습니다."));
            QMessageBox::information(this, QStringLiteral("후보 저장 완료"), status_->text());
        } else QMessageBox::warning(this, QStringLiteral("저장 실패"), error);
    }

    void confirmSavedResult() {
        if (reviewId_.isEmpty() || projectDb_.isEmpty()) return;
        if (QMessageBox::question(this, QStringLiteral("정합 결과 확정"),
            QStringLiteral("검토한 정합 후보를 확정하시겠습니까? 기존 활성 정합을 교체하고, "
                           "선택한 경우 카메라 pose도 모델 좌표계로 갱신합니다.")) != QMessageBox::Yes) return;
        QString error;
        int poseCount = 0;
        if (!ProjectStore::confirmRegistration(projectDb_, reviewId_, &poseCount, &error)) {
            QMessageBox::warning(this, QStringLiteral("확정 실패"), error);
            return;
        }
        reviewId_.clear();
        confirm_->setEnabled(false);
        if (matrix_) {
            savedMatrix_ = vtkSmartPointer<vtkMatrix4x4>::New();
            savedMatrix_->DeepCopy(matrix_);
        }
        savedPoseSameFrame_ = poseSameFrame_->isChecked();
        status_->setText(QStringLiteral("정합 결과 확정 완료 · 카메라 pose %1장 갱신").arg(poseCount));
        QMessageBox::information(this, QStringLiteral("확정 완료"), status_->text());
    }

    QString projectDb_, facilityId_, inspectionId_, modelRoot_, cloudPath_, method_, reviewId_;
    QLineEdit *modelPath_ = nullptr;
    QComboBox *units_ = nullptr, *cloudList_ = nullptr, *modelMode_ = nullptr, *sideMode_ = nullptr;
    int missingSideParts_ = 0;
    QListWidget *partList_ = nullptr;
    QLabel *modelInfo_ = nullptr, *selectedInfo_ = nullptr, *cloudInfo_ = nullptr, *metrics_ = nullptr, *status_ = nullptr;
    QTableWidget *pairs_ = nullptr, *orientationTable_ = nullptr;
    QCheckBox *allowScale_ = nullptr, *poseSameFrame_ = nullptr;
    QPushButton *icpSimilarity_ = nullptr;
    QCheckBox *showPreviewCloud_ = nullptr, *showCameraCenters_ = nullptr, *showSelectedFrustum_ = nullptr;
    QPushButton *previewOriginal_ = nullptr, *previewAligned_ = nullptr;
    QComboBox *previewCamera_ = nullptr, *previewModelMode_ = nullptr;
    QLabel *cameraStatus_ = nullptr;
    QLabel *orientationStatus_ = nullptr;
    QVector<CameraPreviewItem> cameras_;
    PreviewIntrinsics previewIntrinsics_;
    QPushButton *save_ = nullptr, *confirm_ = nullptr;
    QTabWidget *views_ = nullptr;
    PickView *sourceView_ = nullptr, *targetView_ = nullptr, *resultView_ = nullptr;
    CloudPreview cloud_, activeCloud_;
    vtkSmartPointer<vtkPolyData> allCloudGeometry_, cloudGeometry_, model_, selectedModel_;
    vtkSmartPointer<vtkActor> sourceActor_, targetActor_, selectedActor_, overlayCloud_;
    vtkSmartPointer<vtkActor> pendingSourceMarker_, pendingTargetMarker_;
    QVector<vtkSmartPointer<vtkBillboardTextActor3D>> sourceLabels_, targetLabels_;
    QVector<RegistrationPart> parts_;
    vtkSmartPointer<vtkMatrix4x4> matrix_, savedMatrix_, icpStart_, previousMatrix_;
    bool savedPoseSameFrame_ = false;
    QDoubleSpinBox *adjustValues_[6]{};
    QVector3D adjustPivot_;
    QVector<vtkSmartPointer<vtkActor>> sourceMarkers_, targetMarkers_;
    QVector<QVector3D> sourcePairs_, targetPairs_;
    std::optional<QVector3D> pendingSource_, pendingTarget_;
    RegistrationMetrics resultMetrics_;
    OrientationComparison orientationComparison_;
    bool comparisonSimilarity_ = false;
    double landmarkRmse_ = 0;
    int iterations_ = 0;
};
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QString project, facility, inspection, model, cloud, review, capture;
    const auto args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--project") && i + 1 < args.size()) project = args[++i];
        else if (args[i] == QStringLiteral("--facility") && i + 1 < args.size()) facility = args[++i];
        else if (args[i] == QStringLiteral("--inspection") && i + 1 < args.size()) inspection = args[++i];
        else if (args[i] == QStringLiteral("--model") && i + 1 < args.size()) model = args[++i];
        else if (args[i] == QStringLiteral("--cloud") && i + 1 < args.size()) cloud = args[++i];
        else if (args[i] == QStringLiteral("--review") && i + 1 < args.size()) review = args[++i];
        else if (args[i] == QStringLiteral("--capture-ui") && i + 1 < args.size()) capture = args[++i];
    }
    RegistrationWindow window(project, facility, inspection, model, cloud, review);
    window.show();
    if (!capture.isEmpty()) QTimer::singleShot(1800, &window, [&] {
        window.grab().save(capture);
        app.quit();
    });
    return app.exec();
}
