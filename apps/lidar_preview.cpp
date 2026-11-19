#include "lidar_preview.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#ifdef INSPECTMAP_WITH_VTK
#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkNew.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkRenderWindowInteractor.h>
#include <vtkSphereSource.h>
#include <vtkUnsignedCharArray.h>
#endif

namespace {
template <typename T>
T little(const QByteArray &bytes, int offset) {
    return qFromLittleEndian<T>(reinterpret_cast<const uchar *>(bytes.constData() + offset));
}

double lasDouble(const QByteArray &bytes, int offset) {
    return std::bit_cast<double>(little<quint64>(bytes, offset));
}
}

CloudPreview readLasPreview(const QString &path, int maxPoints) {
    CloudPreview result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = file.errorString();
        return result;
    }
    const QByteArray header = file.read(375);
    if (header.size() < 227 || header.left(4) != "LASF") {
        result.error = QStringLiteral("LAS 헤더를 읽을 수 없습니다.");
        return result;
    }
    const quint32 dataOffset = little<quint32>(header, 96);
    const quint8 pointFormat = quint8(header[104]);
    const quint16 recordLength = little<quint16>(header, 105);
    quint64 count = little<quint32>(header, 107);
    if (header.size() >= 255 && little<quint16>(header, 94) >= 255) {
        const quint64 extended = little<quint64>(header, 247);
        if (extended) count = extended;
    }
    if ((pointFormat & 0x80) || recordLength < 14 || dataOffset > quint64(file.size()) ||
        count > (quint64(file.size()) - dataOffset) / recordLength) {
        result.error = QStringLiteral("압축되었거나 손상된 LAS 파일입니다.");
        return result;
    }
    const double scale[3]{lasDouble(header, 131), lasDouble(header, 139), lasDouble(header, 147)};
    const double offset[3]{lasDouble(header, 155), lasDouble(header, 163), lasDouble(header, 171)};
    result.totalPoints = count;
    if (!count) return result;
    const quint64 limit = quint64(std::max(1, maxPoints));
    const quint64 step = std::max<quint64>(1, (count + limit - 1) / limit);
    result.points.reserve(int(std::min<quint64>(count, limit)));
    result.intensities.reserve(result.points.capacity());
    for (quint64 i = 0; i < count; i += step) {
        if (!file.seek(qint64(dataOffset + i * recordLength))) break;
        const QByteArray record = file.read(14);
        if (record.size() != 14) break;
        const double x = double(little<qint32>(record, 0)) * scale[0] + offset[0];
        const double y = double(little<qint32>(record, 4)) * scale[1] + offset[1];
        const double z = double(little<qint32>(record, 8)) * scale[2] + offset[2];
        if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z)) {
            result.points.append(QVector3D(float(x), float(y), float(z)));
            result.intensities.append(little<quint16>(record, 12));
        }
    }
    if (!result.intensities.isEmpty()) {
        auto sorted = result.intensities;
        std::sort(sorted.begin(), sorted.end());
        result.intensityLow = sorted[sorted.size() / 50];
        result.intensityHigh = sorted[sorted.size() * 49 / 50];
    }
    if (result.points.isEmpty()) result.error = QStringLiteral("표시할 점군 좌표가 없습니다.");
    return result;
}

PreviewCamera parsePreviewCamera(const QString &poseJson) {
    PreviewCamera camera;
    const auto document = QJsonDocument::fromJson(poseJson.toUtf8());
    const auto pose = document.object();
    const auto position = pose.value(QStringLiteral("position")).toObject();
    const auto orientation = pose.value(QStringLiteral("orientation")).toObject();
    if (position.isEmpty() || orientation.isEmpty()) return camera;
    camera.position = QVector3D(float(position.value(QStringLiteral("x")).toDouble()),
        float(position.value(QStringLiteral("y")).toDouble()),
        float(position.value(QStringLiteral("z")).toDouble()));
    camera.orientation = QQuaternion(float(orientation.value(QStringLiteral("w")).toDouble()),
        float(orientation.value(QStringLiteral("x")).toDouble()),
        float(orientation.value(QStringLiteral("y")).toDouble()),
        float(orientation.value(QStringLiteral("z")).toDouble())).normalized();
    camera.valid = !camera.orientation.isNull();
    return camera;
}

#ifndef INSPECTMAP_WITH_VTK
LidarPreview::LidarPreview(QWidget *parent) : QWidget(parent) {
    setMinimumSize(300, 300);
    setMouseTracking(true);
}

void LidarPreview::setCloud(CloudPreview cloud) {
    cloud_ = std::move(cloud);
    resetView();
}

void LidarPreview::setCameras(QVector<PreviewCamera> cameras) {
    cameras_ = std::move(cameras);
    update();
}

void LidarPreview::setSelectedCamera(int index) {
    selectedCamera_ = index;
    update();
}

void LidarPreview::setShowCameras(bool show) { showCameras_ = show; update(); }
void LidarPreview::setShowFrustum(bool show) { showFrustum_ = show; update(); }

void LidarPreview::resetView() {
    zoom_ = 1;
    yaw_ = 0.5;
    pitch_ = 0.5;
    if (!cloud_.points.isEmpty()) {
        QVector<float> x, y, z;
        x.reserve(cloud_.points.size());
        y.reserve(cloud_.points.size());
        z.reserve(cloud_.points.size());
        for (const auto &point : cloud_.points) {
            x.append(point.x());
            y.append(point.y());
            z.append(point.z());
        }
        std::sort(x.begin(), x.end());
        std::sort(y.begin(), y.end());
        std::sort(z.begin(), z.end());
        const int margin = cloud_.points.size() / 100;
        const int upperIndex = cloud_.points.size() - 1 - margin;
        const QVector3D lower(x[margin], y[margin], z[margin]);
        const QVector3D upper(x[upperIndex], y[upperIndex], z[upperIndex]);
        center_ = (lower + upper) * 0.5f;
        const auto size = upper - lower;
        extent_ = std::max({qreal(size.x()), qreal(size.y()), qreal(size.z()), 0.1});
    }
    update();
}

QPointF LidarPreview::project(const QVector3D &point) const {
    const QVector3D p = point - center_;
    const qreal x = std::cos(yaw_) * p.x() - std::sin(yaw_) * p.y();
    const qreal y = std::sin(yaw_) * p.x() + std::cos(yaw_) * p.y();
    const qreal vertical = std::cos(pitch_) * p.z() - std::sin(pitch_) * y;
    const qreal scale = std::min(width(), height()) * 0.8 * zoom_ / extent_;
    return {width() * 0.5 + x * scale, height() * 0.5 - vertical * scale};
}

void LidarPreview::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(QStringLiteral("#172b33")));
    painter.setRenderHint(QPainter::Antialiasing, false);
    if (cloud_.points.isEmpty()) {
        painter.setPen(QColor(QStringLiteral("#cbd8db")));
        painter.drawText(rect(), Qt::AlignCenter,
            cloud_.error.isEmpty() ? QStringLiteral("LiDAR 파일을 선택하세요.") : cloud_.error);
        return;
    }
    QPolygonF pixels;
    pixels.reserve(cloud_.points.size());
    for (const auto &point : cloud_.points) pixels.append(project(point));
    painter.setPen(QPen(QColor(QStringLiteral("#a4bdc1")), 1));
    painter.drawPoints(pixels);
    if (showCameras_) {
        painter.setRenderHint(QPainter::Antialiasing);
        for (int i = 0; i < cameras_.size(); ++i) {
            if (!cameras_[i].valid) continue;
            const bool selected = i == selectedCamera_;
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(selected ? QStringLiteral("#f5b63b") : QStringLiteral("#45d6ab")));
            painter.drawEllipse(project(cameras_[i].position), selected ? 5 : 3, selected ? 5 : 3);
        }
    }
    if (showFrustum_ && selectedCamera_ >= 0 && selectedCamera_ < cameras_.size()) {
        const auto &camera = cameras_[selectedCamera_];
        if (camera.valid) {
            const float length = float(std::clamp(extent_ * 0.08, 0.4, 2.0));
            const float halfWidth = length * 0.65f;
            const float halfHeight = halfWidth * 0.75f;
            const QVector3D origin = camera.position;
            const QVector3D forward = camera.orientation.rotatedVector(QVector3D(1, 0, 0));
            const QVector3D right = camera.orientation.rotatedVector(QVector3D(0, 1, 0));
            const QVector3D up = camera.orientation.rotatedVector(QVector3D(0, 0, 1));
            const QVector3D target = origin + forward * length;
            const QVector<QVector3D> corners{target - right * halfWidth + up * halfHeight,
                target + right * halfWidth + up * halfHeight,
                target + right * halfWidth - up * halfHeight,
                target - right * halfWidth - up * halfHeight};
            painter.setPen(QPen(QColor(QStringLiteral("#f5b63b")), 2));
            for (int i = 0; i < 4; ++i) {
                painter.drawLine(project(origin), project(corners[i]));
                painter.drawLine(project(corners[i]), project(corners[(i + 1) % 4]));
            }
        }
    }
}

void LidarPreview::mousePressEvent(QMouseEvent *event) { lastMouse_ = event->pos(); }

void LidarPreview::mouseMoveEvent(QMouseEvent *event) {
    if (event->buttons() & Qt::LeftButton) {
        const QPoint delta = event->pos() - lastMouse_;
        yaw_ += delta.x() * 0.008;
        pitch_ = std::clamp(pitch_ + delta.y() * 0.008, -1.5, 1.5);
        update();
    }
    lastMouse_ = event->pos();
}

void LidarPreview::wheelEvent(QWheelEvent *event) {
    zoom_ = std::clamp(zoom_ * (event->angleDelta().y() > 0 ? 1.2 : 1.0 / 1.2), 0.05, 40.0);
    update();
    event->accept();
}
#else
namespace {
vtkSmartPointer<vtkActor> pointActor(const QVector<QVector3D> &points, const double color[3], double pointSize) {
    vtkNew<vtkPoints> vertices;
    vtkNew<vtkCellArray> cells;
    for (const auto &point : points) {
        const vtkIdType id = vertices->InsertNextPoint(point.x(), point.y(), point.z());
        cells->InsertNextCell(1);
        cells->InsertCellPoint(id);
    }
    vtkNew<vtkPolyData> data;
    data->SetPoints(vertices);
    data->SetVerts(cells);
    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputData(data);
    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetColor(color[0], color[1], color[2]);
    actor->GetProperty()->SetPointSize(pointSize);
    return actor;
}

vtkSmartPointer<vtkActor> intensityActor(const CloudPreview &cloud) {
    constexpr std::array<std::array<double, 3>, 5> palette{{
        {{0.16, 0.18, 0.45}}, {{0.14, 0.51, 0.59}}, {{0.24, 0.72, 0.54}},
        {{0.78, 0.83, 0.35}}, {{0.99, 0.94, 0.66}}}};
    vtkNew<vtkPoints> vertices;
    vtkNew<vtkCellArray> cells;
    vtkNew<vtkUnsignedCharArray> colors;
    colors->SetNumberOfComponents(3);
    colors->SetName("Intensity color");
    const double range = std::max(1.0, double(cloud.intensityHigh) - cloud.intensityLow);
    for (int i = 0; i < cloud.points.size(); ++i) {
        const auto &point = cloud.points[i];
        const vtkIdType id = vertices->InsertNextPoint(point.x(), point.y(), point.z());
        cells->InsertNextCell(1);
        cells->InsertCellPoint(id);
        const double scaled = std::clamp((double(cloud.intensities[i]) - cloud.intensityLow) / range, 0.0, 1.0) * 4.0;
        const int band = std::min(3, int(scaled));
        const double blend = scaled - band;
        unsigned char rgb[3];
        for (int channel = 0; channel < 3; ++channel)
            rgb[channel] = static_cast<unsigned char>(255.0 *
                (palette[band][channel] * (1.0 - blend) + palette[band + 1][channel] * blend));
        colors->InsertNextTypedTuple(rgb);
    }
    vtkNew<vtkPolyData> data;
    data->SetPoints(vertices);
    data->SetVerts(cells);
    data->GetPointData()->SetScalars(colors);
    vtkNew<vtkPolyDataMapper> mapper;
    mapper->SetInputData(data);
    mapper->ScalarVisibilityOn();
    auto actor = vtkSmartPointer<vtkActor>::New();
    actor->SetMapper(mapper);
    actor->GetProperty()->SetPointSize(2.0);
    return actor;
}
}

LidarPreview::LidarPreview(QWidget *parent) : QVTKOpenGLNativeWidget(parent) {
    setMinimumSize(300, 300);
    renderWindow_ = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
    renderer_ = vtkSmartPointer<vtkRenderer>::New();
    renderer_->SetBackground(0.07, 0.13, 0.17);
    renderWindow_->AddRenderer(renderer_);
    setRenderWindow(renderWindow_);
    vtkNew<vtkInteractorStyleTrackballCamera> style;
    interactor()->SetInteractorStyle(style);
}

void LidarPreview::setCloud(CloudPreview cloud) {
    cloud_ = std::move(cloud);
    if (cloudActor_) renderer_->RemoveActor(cloudActor_);
    if (!cloud_.points.isEmpty()) {
        QVector<float> x, y, z;
        x.reserve(cloud_.points.size());
        y.reserve(cloud_.points.size());
        z.reserve(cloud_.points.size());
        for (const auto &point : cloud_.points) {
            x.append(point.x());
            y.append(point.y());
            z.append(point.z());
        }
        std::sort(x.begin(), x.end());
        std::sort(y.begin(), y.end());
        std::sort(z.begin(), z.end());
        const int margin = cloud_.points.size() / 100;
        const int high = cloud_.points.size() - 1 - margin;
        const QVector3D lower(x[margin], y[margin], z[margin]);
        const QVector3D upper(x[high], y[high], z[high]);
        robustCenter_ = (lower + upper) * 0.5f;
        const QVector3D size = upper - lower;
        robustExtent_ = std::max({double(size.x()), double(size.y()), double(size.z()), 0.1});
        cloudActor_ = intensityActor(cloud_);
        renderer_->AddActor(cloudActor_);
    } else cloudActor_ = nullptr;
    resetView();
}

void LidarPreview::setCameras(QVector<PreviewCamera> cameras) {
    cameras_ = std::move(cameras);
    updateCameras();
}

void LidarPreview::setSelectedCamera(int index) {
    selectedCamera_ = index;
    updateSelection();
}

void LidarPreview::setShowCameras(bool show) {
    showCameras_ = show;
    if (cameraActor_) cameraActor_->SetVisibility(show);
    if (selectedActor_) selectedActor_->SetVisibility(show);
    renderScene();
}

void LidarPreview::setShowFrustum(bool show) {
    showFrustum_ = show;
    if (frustumActor_) frustumActor_->SetVisibility(show);
    renderScene();
}

void LidarPreview::resetView() {
    auto *camera = renderer_->GetActiveCamera();
    camera->SetFocalPoint(robustCenter_.x(), robustCenter_.y(), robustCenter_.z());
    camera->SetPosition(robustCenter_.x() + robustExtent_ * 1.8,
        robustCenter_.y() - robustExtent_ * 2.5,
        robustCenter_.z() + robustExtent_ * 0.7);
    camera->SetViewUp(0, 0, 1);
    renderer_->GetActiveCamera()->OrthogonalizeViewUp();
    renderer_->ResetCameraClippingRange();
    updateSelection();
    renderScene();
}

void LidarPreview::updateCameras() {
    if (cameraActor_) renderer_->RemoveActor(cameraActor_);
    QVector<QVector3D> positions;
    for (const auto &camera : cameras_) if (camera.valid) positions.append(camera.position);
    const double color[]{0.95, 0.20, 0.18};
    cameraActor_ = pointActor(positions, color, 10.0);
    cameraActor_->SetVisibility(showCameras_);
    renderer_->AddActor(cameraActor_);
    updateSelection();
}

void LidarPreview::updateSelection() {
    if (selectedActor_) renderer_->RemoveActor(selectedActor_);
    if (frustumActor_) renderer_->RemoveActor(frustumActor_);
    selectedActor_ = nullptr;
    frustumActor_ = nullptr;
    if (selectedCamera_ < 0 || selectedCamera_ >= cameras_.size() || !cameras_[selectedCamera_].valid) {
        renderScene();
        return;
    }
    const auto &camera = cameras_[selectedCamera_];
    const double extent = robustExtent_;
    const double length = std::clamp(extent * 0.08, 0.4, 2.0);
    vtkNew<vtkSphereSource> sphere;
    sphere->SetCenter(camera.position.x(), camera.position.y(), camera.position.z());
    sphere->SetRadius(std::max(0.08, extent * 0.01));
    sphere->SetThetaResolution(12);
    sphere->SetPhiResolution(12);
    vtkNew<vtkPolyDataMapper> sphereMapper;
    sphereMapper->SetInputConnection(sphere->GetOutputPort());
    selectedActor_ = vtkSmartPointer<vtkActor>::New();
    selectedActor_->SetMapper(sphereMapper);
    selectedActor_->GetProperty()->SetColor(0.98, 0.70, 0.20);
    selectedActor_->SetVisibility(showCameras_);
    renderer_->AddActor(selectedActor_);

    const QVector3D origin = camera.position;
    const QVector3D forward = camera.orientation.rotatedVector(QVector3D(1, 0, 0));
    const QVector3D right = camera.orientation.rotatedVector(QVector3D(0, 1, 0));
    const QVector3D up = camera.orientation.rotatedVector(QVector3D(0, 0, 1));
    const QVector3D target = origin + forward * float(length);
    const float halfWidth = float(length * 0.65);
    const float halfHeight = halfWidth * 0.75f;
    const QVector<QVector3D> corners{target - right * halfWidth + up * halfHeight,
        target + right * halfWidth + up * halfHeight,
        target + right * halfWidth - up * halfHeight,
        target - right * halfWidth - up * halfHeight};
    vtkNew<vtkPoints> points;
    points->InsertNextPoint(origin.x(), origin.y(), origin.z());
    for (const auto &corner : corners) points->InsertNextPoint(corner.x(), corner.y(), corner.z());
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
    frustumActor_ = vtkSmartPointer<vtkActor>::New();
    frustumActor_->SetMapper(mapper);
    frustumActor_->GetProperty()->SetColor(0.98, 0.70, 0.20);
    frustumActor_->GetProperty()->SetLineWidth(2);
    frustumActor_->SetVisibility(showFrustum_);
    renderer_->AddActor(frustumActor_);
    renderScene();
}

void LidarPreview::renderScene() {
    if (isValid()) renderWindow_->Render();
    else update();
}
#endif
