#pragma once

#include <QQuaternion>
#include <QVector3D>
#include <QVector>
#include <QWidget>
#ifdef INSPECTMAP_WITH_VTK
#include <QVTKOpenGLNativeWidget.h>
#include <vtkSmartPointer.h>
class vtkActor;
class vtkGenericOpenGLRenderWindow;
class vtkRenderer;
#endif

struct CloudPreview {
    QVector<QVector3D> points;
    QVector<quint16> intensities;
    quint64 totalPoints = 0;
    quint16 intensityLow = 0;
    quint16 intensityHigh = 0;
    QString error;
};

struct PreviewCamera {
    QVector3D position;
    QQuaternion orientation;
    bool valid = false;
};

CloudPreview readLasPreview(const QString &path, int maxPoints = 300000);
PreviewCamera parsePreviewCamera(const QString &poseJson);

class LidarPreview : public
#ifdef INSPECTMAP_WITH_VTK
    QVTKOpenGLNativeWidget {
#else
    QWidget {
#endif
public:
    explicit LidarPreview(QWidget *parent = nullptr);
    void setCloud(CloudPreview cloud);
    void setCameras(QVector<PreviewCamera> cameras);
    void setSelectedCamera(int index);
    void setShowCameras(bool show);
    void setShowFrustum(bool show);
    void resetView();

#ifndef INSPECTMAP_WITH_VTK
protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    QPointF project(const QVector3D &point) const;
#else
private:
    void updateCameras();
    void updateSelection();
    void renderScene();
    vtkSmartPointer<vtkGenericOpenGLRenderWindow> renderWindow_;
    vtkSmartPointer<vtkRenderer> renderer_;
    vtkSmartPointer<vtkActor> cloudActor_;
    vtkSmartPointer<vtkActor> cameraActor_;
    vtkSmartPointer<vtkActor> selectedActor_;
    vtkSmartPointer<vtkActor> frustumActor_;
    QVector3D robustCenter_;
    double robustExtent_ = 10.0;
#endif
    CloudPreview cloud_;
    QVector<PreviewCamera> cameras_;
    int selectedCamera_ = -1;
#ifndef INSPECTMAP_WITH_VTK
    QVector3D center_;
    QPoint lastMouse_;
    qreal extent_ = 1;
    qreal zoom_ = 1;
    qreal yaw_ = 0.5;
    qreal pitch_ = 0.5;
#endif
    bool showCameras_ = true;
    bool showFrustum_ = true;
};
