#include "conversion_core.h"
#include "damage_store.h"
#include "project_store.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsPathItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHeaderView>
#include <QHash>
#include <QImageReader>
#include <QImageIOHandler>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRubberBand>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QStyleOptionGraphicsItem>
#include <QTableWidget>
#include <QTimer>
#include <QToolBar>
#include <QTransform>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QMainWindow>

#include <algorithm>
#include <functional>

namespace {
QString fmt(const QJsonValue &value) {
    if (value.isNull() || value.isUndefined()) return QStringLiteral("—");
    if (value.isDouble()) return QString::number(value.toDouble(), 'f', 2);
    return value.toString();
}
QString quantity(const QJsonObject &value, const QString &metric, const QString &unit) {
    const auto amount = value.value(metric).toObject().value(unit);
    return amount.isDouble() ? fmt(amount) + QStringLiteral(" ") + unit : QStringLiteral("—");
}
QColor colorFor(const DamageObject &object) {
    if (object.reviewState == QStringLiteral("REJECTED")) return QColor(120, 130, 140);
    const QString type = object.overrideType.isEmpty() ? object.type : object.overrideType;
    if (type.startsWith(QStringLiteral("target_"))) return QColor(64, 150, 210);
    if (type == QStringLiteral("crack") || type == QStringLiteral("crack_region")) return QColor(231, 56, 66);
    return QColor(235, 151, 38);
}
QPainterPath linePath(const QJsonArray &lines) {
    QPainterPath path;
    for (const auto &entry : lines) {
        const auto points = entry.toArray();
        bool started = false;
        for (const auto &point : points) {
            const auto xy = point.toArray();
            if (xy.size() < 2) continue;
            if (!started) { path.moveTo(xy[0].toDouble(), xy[1].toDouble()); started = true; }
            else path.lineTo(xy[0].toDouble(), xy[1].toDouble());
        }
    }
    return path;
}
QPainterPath polygonPath(const QJsonArray &rings) {
    QPainterPath path;
    path.setFillRule(Qt::OddEvenFill);
    for (const auto &ring : rings) {
        const auto points = ring.toArray();
        bool started = false;
        for (const auto &point : points) {
            const auto xy = point.toArray();
            if (xy.size() < 2) continue;
            if (!started) { path.moveTo(xy[0].toDouble(), xy[1].toDouble()); started = true; }
            else path.lineTo(xy[0].toDouble(), xy[1].toDouble());
        }
        if (started) path.closeSubpath();
    }
    return path;
}
QPainterPath boxPath(const QJsonArray &box) {
    QPainterPath path;
    if (box.size() == 4) path.addRect(QRectF(QPointF(box[0].toDouble(), box[1].toDouble()),
                                      QPointF(box[2].toDouble(), box[3].toDouble())).normalized());
    return path;
}
QString groupFor(const QString &type) {
    if (type.startsWith(QStringLiteral("target_"))) return QStringLiteral("표적지");
    if (type == QStringLiteral("crack") || type == QStringLiteral("crack_region")) return QStringLiteral("균열");
    return QStringLiteral("면형 손상");
}
QString displayId(const DamageObject &object) {
    return object.sourceId == QStringLiteral("manual") ?
        QStringLiteral("manual-") + object.id.left(8) : object.sourceId;
}
QString reviewLabel(const DamageObject &object, bool edited) {
    if (object.type.startsWith(QStringLiteral("target_")) && !edited && !object.reviewConfirmed)
        return QStringLiteral("참조");
    const QString state = object.reviewState == QStringLiteral("ACCEPTED") ? QStringLiteral("채택") :
        object.reviewState == QStringLiteral("REJECTED") ? QStringLiteral("제외") :
        object.reviewState == QStringLiteral("HOLD") ? QStringLiteral("보류") : QStringLiteral("미검토");
    return state + (edited ? QStringLiteral(" · 수정") :
        object.reviewConfirmed ? QString() : QStringLiteral(" · 제안"));
}
QTransform imageOrientation(QImageIOHandler::Transformations flags, const QSize &size) {
    const double width = size.width(), height = size.height();
    switch (int(flags)) {
    case 1: return QTransform(-1, 0, 0, 1, width, 0);
    case 2: return QTransform(1, 0, 0, -1, 0, height);
    case 3: return QTransform(-1, 0, 0, -1, width, height);
    case 4: return QTransform(0, 1, -1, 0, height, 0);
    case 5: return QTransform(0, 1, 1, 0, 0, 0);
    case 6: return QTransform(0, -1, -1, 0, height, width);
    case 7: return QTransform(0, -1, 1, 0, 0, width);
    default: return {};
    }
}
}

class DamagePathItem : public QGraphicsPathItem {
public:
    using QGraphicsPathItem::QGraphicsPathItem;

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override {
        QStyleOptionGraphicsItem unselected(*option);
        unselected.state &= ~QStyle::State_Selected;
        QGraphicsPathItem::paint(painter, &unselected, widget);
    }
};

class ImageView : public QGraphicsView {
public:
    explicit ImageView(QWidget *parent = nullptr) : QGraphicsView(parent) {
        setRenderHint(QPainter::Antialiasing);
        setDragMode(QGraphicsView::ScrollHandDrag);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        rubber_ = new QRubberBand(QRubberBand::Rectangle, viewport());
    }
    void fitImage() {
        if (fitting_ || !scene() || scene()->sceneRect().isEmpty()) return;
        fitting_ = true;
        resetTransform();
        fitInView(scene()->sceneRect(), Qt::KeepAspectRatio);
        autoFit_ = true;
        fitting_ = false;
    }
    void zoom(double factor) { scale(factor, factor); autoFit_ = false; }
    void focusOn(const QRectF &bounds) {
        if (!scene() || !bounds.isValid() || bounds.isEmpty()) return;
        const QSizeF size(std::max(250.0, bounds.width() * 1.4),
                          std::max(200.0, bounds.height() * 1.4));
        const QRectF focus(bounds.center() - QPointF(size.width() / 2, size.height() / 2), size);
        autoFit_ = false;
        resetTransform();
        fitInView(focus, Qt::KeepAspectRatio);
        centerOn(bounds.center());
    }
    void selectRegion(std::function<void(const QRectF &)> handler) {
        regionHandler_ = std::move(handler);
        setDragMode(QGraphicsView::NoDrag);
        viewport()->setCursor(Qt::CrossCursor);
    }
protected:
    void wheelEvent(QWheelEvent *event) override {
        zoom(event->angleDelta().y() > 0 ? 1.2 : 1.0 / 1.2);
        event->accept();
    }
    void resizeEvent(QResizeEvent *event) override {
        QGraphicsView::resizeEvent(event);
        if (autoFit_) fitImage();
    }
    void mousePressEvent(QMouseEvent *event) override {
        if (regionHandler_ && event->button() == Qt::LeftButton) {
            start_ = event->pos();
            rubber_->setGeometry(QRect(start_, QSize()));
            rubber_->show();
            return;
        }
        QGraphicsView::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (rubber_->isVisible()) { rubber_->setGeometry(QRect(start_, event->pos()).normalized()); return; }
        QGraphicsView::mouseMoveEvent(event);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (rubber_->isVisible() && event->button() == Qt::LeftButton) {
            const QRect rect = rubber_->geometry();
            rubber_->hide();
            setDragMode(QGraphicsView::ScrollHandDrag);
            viewport()->unsetCursor();
            auto handler = std::move(regionHandler_);
            regionHandler_ = {};
            if (rect.width() > 4 && rect.height() > 4 && handler)
                handler(mapToScene(rect).boundingRect());
            return;
        }
        QGraphicsView::mouseReleaseEvent(event);
    }
private:
    QRubberBand *rubber_ = nullptr;
    QPoint start_;
    bool autoFit_ = true;
    bool fitting_ = false;
    std::function<void(const QRectF &)> regionHandler_;
};

class DamageWindow : public QMainWindow {
public:
    ~DamageWindow() override {
        QObject::disconnect(scene_, nullptr, this, nullptr);
        QObject::disconnect(images_, nullptr, this, nullptr);
        QObject::disconnect(objects_, nullptr, this, nullptr);
        QObject::disconnect(dataset_, nullptr, this, nullptr);
    }
    DamageWindow(const QString &project, const QString &inspection) : projectDb_(project), inspectionId_(inspection) {
        setWindowTitle(QStringLiteral("InspectMap | AI 손상정보 관리자"));
        resize(1650, 960);
        auto *root = new QWidget;
        auto *layout = new QVBoxLayout(root);
        layout->setContentsMargins(16, 12, 16, 12);
        auto *title = new QLabel(QStringLiteral("InspectMap · AI 손상정보 관리자"));
        title->setStyleSheet(QStringLiteral("background:#243c4e;color:white;font-size:22px;font-weight:bold;padding:14px;"));
        layout->addWidget(title);
        auto *projectBar = new QHBoxLayout;
        projectLabel_ = new QLabel;
        auto *chooseProject = new QPushButton(QStringLiteral("프로젝트 열기"));
        projectBar->addWidget(projectLabel_, 1);
        projectBar->addWidget(chooseProject);
        layout->addLayout(projectBar);
        auto *sourceBar = new QHBoxLayout;
        sourceBar->addWidget(new QLabel(QStringLiteral("데이터 세트")));
        dataset_ = new QComboBox;
        dataset_->setMinimumWidth(220);
        sourceBar->addWidget(dataset_);
        folder_ = new QLineEdit;
        folder_->setPlaceholderText(QStringLiteral("탐지 결과 JSON 폴더"));
        sourceBar->addWidget(folder_, 1);
        auto *browse = new QPushButton(QStringLiteral("폴더 선택"));
        auto *import = new QPushButton(QStringLiteral("오프라인 결과 가져오기"));
        import->setObjectName(QStringLiteral("primary"));
        auto *server = new QPushButton(QStringLiteral("AI 서버 요청"));
        server->setEnabled(false);
        server->setToolTip(QStringLiteral("AI 서버 API 연동 후 활성화됩니다."));
        sourceBar->addWidget(browse);
        sourceBar->addWidget(import);
        sourceBar->addWidget(server);
        layout->addLayout(sourceBar);

        auto *split = new QSplitter;
        auto *left = new QWidget;
        auto *leftLayout = new QVBoxLayout(left);
        leftLayout->setContentsMargins(0, 0, 0, 0);
        leftLayout->addWidget(new QLabel(QStringLiteral("영상 · 탐지 결과")));
        images_ = new QTableWidget(0, 2);
        images_->setHorizontalHeaderLabels({QStringLiteral("영상"), QStringLiteral("객체")});
        images_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        images_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
        images_->setColumnWidth(1, 58);
        images_->verticalHeader()->hide();
        images_->setSelectionBehavior(QAbstractItemView::SelectRows);
        images_->setSelectionMode(QAbstractItemView::SingleSelection);
        images_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        leftLayout->addWidget(images_, 1);
        split->addWidget(left);

        auto *center = new QWidget;
        auto *centerLayout = new QVBoxLayout(center);
        centerLayout->setContentsMargins(0, 0, 0, 0);
        auto *viewBar = new QHBoxLayout;
        imageSummary_ = new QLabel(QStringLiteral("영상을 선택하십시오."));
        viewBar->addWidget(imageSummary_, 1);
        auto *zoomIn = new QPushButton(QStringLiteral("확대"));
        auto *zoomOut = new QPushButton(QStringLiteral("축소"));
        auto *fit = new QPushButton(QStringLiteral("맞춤"));
        viewBar->addWidget(zoomIn); viewBar->addWidget(zoomOut); viewBar->addWidget(fit);
        centerLayout->addLayout(viewBar);
        auto *filterBar = new QHBoxLayout;
        filterBar->addWidget(new QLabel(QStringLiteral("표시")));
        typeFilter_ = new QComboBox;
        typeFilter_->addItems({QStringLiteral("전체"), QStringLiteral("균열"),
            QStringLiteral("면형 손상"), QStringLiteral("표적지")});
        reviewFilter_ = new QComboBox;
        reviewFilter_->addItems({QStringLiteral("전체 판정"), QStringLiteral("검토 필요"),
            QStringLiteral("채택"), QStringLiteral("보류"), QStringLiteral("제외")});
        filterBar->addWidget(typeFilter_);
        filterBar->addWidget(reviewFilter_);
        filterBar->addStretch();
        centerLayout->addLayout(filterBar);
        view_ = new ImageView;
        scene_ = new QGraphicsScene(view_);
        view_->setScene(scene_);
        view_->setBackgroundBrush(QColor(27, 39, 47));
        centerLayout->addWidget(view_, 1);
        split->addWidget(center);

        auto *right = new QWidget;
        auto *rightLayout = new QVBoxLayout(right);
        rightLayout->setContentsMargins(0, 0, 0, 0);
        rightLayout->addWidget(new QLabel(QStringLiteral("손상 객체")));
        objects_ = new QTableWidget(0, 4);
        objects_->setHorizontalHeaderLabels({QStringLiteral("ID"), QStringLiteral("유형"),
            QStringLiteral("LLM"), QStringLiteral("검토")});
        objects_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        for (int column = 1; column < 4; ++column)
            objects_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Fixed);
        objects_->setColumnWidth(1, 80);
        objects_->setColumnWidth(2, 42);
        objects_->setColumnWidth(3, 86);
        objects_->setMinimumHeight(220);
        objects_->verticalHeader()->hide();
        objects_->setSelectionBehavior(QAbstractItemView::SelectRows);
        objects_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        objects_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        rightLayout->addWidget(objects_, 1);
        detail_ = new QLabel;
        detail_->setWordWrap(true);
        detail_->setMinimumHeight(100);
        detail_->setMaximumHeight(130);
        rightLayout->addWidget(detail_);
        reason_ = new QPlainTextEdit;
        reason_->setReadOnly(true);
        reason_->setMaximumHeight(90);
        rightLayout->addWidget(reason_);
        auto *reviewForm = new QFormLayout;
        review_ = new QComboBox;
        review_->addItem(QStringLiteral("미검토"), QStringLiteral("UNREVIEWED"));
        review_->addItem(QStringLiteral("채택"), QStringLiteral("ACCEPTED"));
        review_->addItem(QStringLiteral("보류"), QStringLiteral("HOLD"));
        review_->addItem(QStringLiteral("제외"), QStringLiteral("REJECTED"));
        review_->addItem(QStringLiteral("혼합"), QString());
        type_ = new QComboBox;
        type_->setEditable(true);
        type_->addItems({QString(), QStringLiteral("crack"), QStringLiteral("crack_region"),
            QStringLiteral("leakage_wetspot"), QStringLiteral("efflorescence"),
            QStringLiteral("spalling"), QStringLiteral("exposed_rebar")});
        note_ = new QLineEdit;
        reviewForm->addRow(QStringLiteral("판정"), review_);
        reviewForm->addRow(QStringLiteral("유형 수정"), type_);
        reviewForm->addRow(QStringLiteral("메모"), note_);
        rightLayout->addLayout(reviewForm);
        auto *saveReview = new QPushButton(QStringLiteral("검토 저장"));
        saveReview_ = saveReview;
        saveReview_->setEnabled(false);
        auto *add = new QPushButton(QStringLiteral("영역 선택하여 손상 추가"));
        rightLayout->addWidget(saveReview);
        rightLayout->addWidget(add);
        split->addWidget(right);
        split->setSizes({290, 860, 450});
        layout->addWidget(split, 1);
        setCentralWidget(root);
        setStyleSheet(QStringLiteral("QWidget {font-size:13px;} QMainWindow,QWidget {background:#f3f6f7;color:#183242;} "
            "QTableWidget,QGraphicsView,QPlainTextEdit,QLineEdit,QComboBox {background:white;border:1px solid #c5d3d8;} "
            "QTableWidget {selection-background-color:#dcece8;selection-color:#173b37;} "
            "QTableWidget::item:selected {background:#dcece8;color:#173b37;border:0;border-radius:0;} "
            "QTableWidget::item:focus {outline:none;border:0;} "
            "QPushButton {background:white;border:1px solid #b9cbd1;padding:6px 10px;} "
            "QPushButton:hover {background:#e6f2f0;} QPushButton#primary {background:#0f766e;color:white;}"));
        statusBar()->showMessage(QStringLiteral("프로젝트를 여십시오."));

        connect(chooseProject, &QPushButton::clicked, this, [this] {
            const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("프로젝트 DB"), {},
                QStringLiteral("SQLite (*.sqlite)"));
            if (!path.isEmpty()) openProject(path);
        });
        connect(browse, &QPushButton::clicked, this, [this] {
            const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("탐지 JSON 폴더"), folder_->text());
            if (!path.isEmpty()) folder_->setText(path);
        });
        connect(import, &QPushButton::clicked, this, [this] { importResults(); });
        connect(dataset_, &QComboBox::currentIndexChanged, this, [this] { refreshImages(); });
        connect(images_, &QTableWidget::currentCellChanged, this, [this](int row) { showImage(row); });
        connect(objects_, &QTableWidget::currentCellChanged, this, [this](int row) { selectObject(row); });
        connect(objects_, &QTableWidget::itemSelectionChanged, this, [this] {
            selectObject(objects_->currentRow());
        });
        connect(objects_, &QTableWidget::cellDoubleClicked, this, [this](int row) {
            if (row < 0 || row >= objectsData_.size()) return;
            const auto &bbox = objectsData_[row].bbox;
            if (bbox.size() != 4) return;
            view_->focusOn(orientationTransform_.mapRect(QRectF(
                QPointF(bbox[0].toDouble(), bbox[1].toDouble()),
                QPointF(bbox[2].toDouble(), bbox[3].toDouble())).normalized()));
        });
        connect(scene_, &QGraphicsScene::selectionChanged, this, [this] {
            const auto selected = scene_->selectedItems();
            if (!selected.isEmpty()) objects_->selectRow(selected.first()->data(0).toInt());
        });
        connect(typeFilter_, &QComboBox::currentIndexChanged, this, [this] { refreshObjects(); });
        connect(reviewFilter_, &QComboBox::currentIndexChanged, this, [this] { refreshObjects(); });
        connect(zoomIn, &QPushButton::clicked, this, [this] { view_->zoom(1.25); });
        connect(zoomOut, &QPushButton::clicked, this, [this] { view_->zoom(0.8); });
        connect(fit, &QPushButton::clicked, this, [this] { view_->fitImage(); });
        connect(review_, &QComboBox::currentIndexChanged, this, [this] { stageSelectedReview(true); });
        connect(type_, &QComboBox::currentTextChanged, this, [this] { stageSelectedReview(false); });
        connect(note_, &QLineEdit::textChanged, this, [this] { stageSelectedReview(false); });
        connect(saveReview, &QPushButton::clicked, this, [this] { saveImageReviews(); });
        connect(add, &QPushButton::clicked, this, [this] { addManual(); });
        if (!project.isEmpty()) openProject(project);
    }

protected:
    void closeEvent(QCloseEvent *event) override {
        if (!drafts_.isEmpty() && QMessageBox::question(this, QStringLiteral("미저장 검토"),
                QStringLiteral("저장하지 않은 영상 검토가 있습니다. 종료하면 변경 사항이 사라집니다. 종료하시겠습니까?"))
                != QMessageBox::Yes) { event->ignore(); return; }
        QMainWindow::closeEvent(event);
    }

private:
    void openProject(const QString &path) {
        if (path != projectDb_ && !drafts_.isEmpty() &&
            QMessageBox::question(this, QStringLiteral("미저장 검토"),
                QStringLiteral("다른 프로젝트를 열면 저장하지 않은 검토가 사라집니다. 계속하시겠습니까?"))
                != QMessageBox::Yes) return;
        QString error;
        ProjectInfo info;
        if (!ProjectStore::read(path, &info, &error) ||
            !ConversionCore::ensureSchema(path, &error) || !DamageStore::ensureSchema(path, &error)) {
            QMessageBox::warning(this, QStringLiteral("프로젝트 열기 실패"), error); return;
        }
        if (path != projectDb_) drafts_.clear();
        projectDb_ = path;
        projectLabel_->setText(info.name);
        projectLabel_->setToolTip(path);
        QList<DamageDataset> items;
        if (!DamageStore::datasets(path, inspectionId_, &items, &error)) {
            QMessageBox::warning(this, QStringLiteral("데이터 세트 조회 실패"), error); return;
        }
        {
            const QSignalBlocker blocker(dataset_);
            dataset_->clear();
            for (const auto &item : items)
                dataset_->addItem(QStringLiteral("%1 · %2장").arg(item.name).arg(item.imageCount), item.id);
        }
        refreshImages();
    }
    void refreshImages() {
        imagesData_.clear();
        objectsData_.clear();
        {
            const QSignalBlocker sceneBlocker(scene_);
            const QSignalBlocker objectBlocker(objects_);
            selectionHighlights_.clear();
            scene_->clear();
            objects_->setRowCount(0);
        }
        images_->setRowCount(0);
        const QString datasetId = dataset_->currentData().toString();
        if (projectDb_.isEmpty() || datasetId.isEmpty()) return;
        QString error;
        if (!DamageStore::images(projectDb_, datasetId, &imagesData_, &error)) {
            statusBar()->showMessage(error); return;
        }
        images_->setRowCount(imagesData_.size());
        for (int row = 0; row < imagesData_.size(); ++row) {
            images_->setItem(row, 0, new QTableWidgetItem(imagesData_[row].name));
            images_->setItem(row, 1, new QTableWidgetItem(imagesData_[row].resultId.isEmpty() ?
                QStringLiteral("—") : QString::number(imagesData_[row].objectCount)));
        }
        if (!imagesData_.isEmpty()) images_->selectRow(0);
        updateSaveButton();
        statusBar()->showMessage(QStringLiteral("영상 %1장 · 탐지 결과 %2장")
            .arg(imagesData_.size()).arg(std::count_if(imagesData_.begin(), imagesData_.end(),
                [](const DamageImage &image) { return !image.resultId.isEmpty(); })));
    }
    void showImage(int row) {
        {
            const QSignalBlocker sceneBlocker(scene_);
            const QSignalBlocker objectBlocker(objects_);
            selectionHighlights_.clear();
            scene_->clear();
            objectsData_.clear();
            objects_->setRowCount(0);
        }
        orientationTransform_ = {};
        detail_->clear(); reason_->clear();
        if (row < 0 || row >= imagesData_.size()) return;
        const auto &image = imagesData_[row];
        QImageReader reader(image.path);
        reader.setAutoTransform(true);
        orientationTransform_ = imageOrientation(reader.transformation(), QSize(image.width, image.height));
        const QImage pixels = reader.read();
        if (pixels.isNull()) {
            imageSummary_->setText(image.name + QStringLiteral(" · 영상 파일 없음"));
            return;
        }
        scene_->addPixmap(QPixmap::fromImage(pixels))->setZValue(-1);
        scene_->setSceneRect(QRectF(QPointF(0, 0), pixels.size()));
        imageSummary_->setText(QStringLiteral("%1 · %2×%3 · GSD %4 mm/px · 객체 %5")
            .arg(image.name).arg(image.width).arg(image.height).arg(image.gsd, 0, 'f', 4)
            .arg(image.objectCount));
        view_->fitImage();
        if (!image.resultId.isEmpty()) refreshObjects();
        updateSaveButton();
    }
    void refreshObjects() {
        const int imageRow = images_->currentRow();
        if (imageRow < 0 || imageRow >= imagesData_.size()) return;
        const QString selectedId = objects_->currentRow() >= 0 && objects_->currentRow() < objectsData_.size() ?
            objectsData_[objects_->currentRow()].id : QString();
        QSet<QString> selectedIds;
        for (const auto &index : objects_->selectionModel()->selectedRows())
            if (index.row() >= 0 && index.row() < objectsData_.size())
                selectedIds.insert(objectsData_[index.row()].id);
        const QString resultId = imagesData_[imageRow].resultId;
        QString error;
        QList<DamageObject> all;
        if (!resultId.isEmpty() && !DamageStore::objects(projectDb_, resultId, &all, &error)) {
            statusBar()->showMessage(error); return;
        }
        objectsData_.clear();
        for (auto object : all) {
            const auto draft = drafts_.value(resultId).value(object.id);
            if (!draft.objectId.isEmpty()) {
                object.reviewState = draft.state;
                object.overrideType = draft.type;
                object.note = draft.note;
            }
            if (typeFilter_->currentText() != QStringLiteral("전체") &&
                groupFor(object.overrideType.isEmpty() ? object.type : object.overrideType) != typeFilter_->currentText()) continue;
            const int reviewIndex = reviewFilter_->currentIndex();
            const QStringList states{QString(), QString(), QStringLiteral("ACCEPTED"),
                QStringLiteral("HOLD"), QStringLiteral("REJECTED")};
            if (reviewIndex == 1 && (object.reviewConfirmed || object.type.startsWith(QStringLiteral("target_")))) continue;
            if (reviewIndex > 1 && object.reviewState != states[reviewIndex]) continue;
            objectsData_.append(object);
        }
        const QSignalBlocker tableBlocker(objects_);
        objects_->setRowCount(objectsData_.size());
        for (int row = 0; row < objectsData_.size(); ++row) {
            const auto &object = objectsData_[row];
            const QStringList cells{displayId(object), object.overrideType.isEmpty() ? object.label : object.overrideType,
                object.llmReview.value(QStringLiteral("label")).toString(QStringLiteral("—")),
                reviewLabel(object, drafts_.value(resultId).contains(object.id))};
            for (int col = 0; col < cells.size(); ++col) objects_->setItem(row, col, new QTableWidgetItem(cells[col]));
        }
        {
            const QSignalBlocker blocker(scene_);
            selectionHighlights_.clear();
            for (auto *item : scene_->items()) if (item->zValue() >= 0) {
                scene_->removeItem(item);
                delete item;
            }
            for (int row = 0; row < objectsData_.size(); ++row) drawObject(objectsData_[row], row);
        }
        int selectedRow = -1;
        objects_->selectionModel()->clearSelection();
        for (int row = 0; row < objectsData_.size(); ++row) {
            if (!selectedIds.contains(objectsData_[row].id)) continue;
            objects_->selectionModel()->select(objects_->model()->index(row, 0),
                QItemSelectionModel::Select | QItemSelectionModel::Rows);
            if (objectsData_[row].id == selectedId) selectedRow = row;
            else if (selectedRow < 0) selectedRow = row;
        }
        if (selectedRow < 0 && !objectsData_.isEmpty()) {
            selectedRow = 0;
            objects_->selectionModel()->select(objects_->model()->index(0, 0),
                QItemSelectionModel::Select | QItemSelectionModel::Rows);
        }
        if (selectedRow >= 0)
            objects_->selectionModel()->setCurrentIndex(objects_->model()->index(selectedRow, 0),
                QItemSelectionModel::NoUpdate);
        else { detail_->clear(); reason_->clear(); }
        selectObject(objects_->currentRow());
        updateSaveButton();
    }
    void drawObject(const DamageObject &object, int row) {
        const QColor color = colorFor(object);
        auto addPath = [&](const QPainterPath &path, bool filled) {
            if (path.isEmpty()) return;
            QPen pen(color, 2);
            pen.setCosmetic(true);
            auto *item = new DamagePathItem(orientationTransform_.map(path));
            item->setPen(pen);
            item->setBrush(filled ? QBrush(QColor(color.red(), color.green(), color.blue(), 45)) : QBrush(Qt::NoBrush));
            scene_->addItem(item);
            item->setData(0, row);
            item->setFlag(QGraphicsItem::ItemIsSelectable);
            item->setZValue(1);
        };
        const auto geometry = object.geometry;
        bool drawn = false;
        const auto regions = geometry.value(QStringLiteral("regions")).toArray();
        for (const auto &region : regions) {
            const auto path = polygonPath(region.toObject().value(QStringLiteral("polygon")).toArray());
            if (!path.isEmpty()) { addPath(path, true); drawn = true; }
        }
        const auto polygon = polygonPath(geometry.value(QStringLiteral("polygon")).toArray());
        if (!polygon.isEmpty()) { addPath(polygon, true); drawn = true; }
        const auto lines = linePath(geometry.value(QStringLiteral("lines")).toArray());
        if (!lines.isEmpty()) { addPath(lines, false); drawn = true; }
        if (!drawn) addPath(boxPath(object.bbox), false);
    }
    void selectObject(int row) {
        for (auto *highlight : selectionHighlights_) {
            scene_->removeItem(highlight);
            delete highlight;
        }
        selectionHighlights_.clear();
        QSet<int> selectedRows;
        for (const auto &index : objects_->selectionModel()->selectedRows())
            if (index.row() >= 0 && index.row() < objectsData_.size()) selectedRows.insert(index.row());
        if (selectedRows.isEmpty() && row >= 0 && row < objectsData_.size()) selectedRows.insert(row);
        const QSignalBlocker reviewBlocker(review_);
        const QSignalBlocker typeBlocker(type_);
        const QSignalBlocker noteBlocker(note_);
        review_->setEnabled(!selectedRows.isEmpty());
        const bool single = selectedRows.size() == 1;
        type_->setEnabled(single);
        note_->setEnabled(single);
        if (selectedRows.isEmpty()) { detail_->clear(); reason_->clear(); return; }
        if (!selectedRows.contains(row)) row = *selectedRows.begin();
        const auto &object = objectsData_[row];
        if (single) {
            const auto measurements = object.measurements;
            const auto llm = object.llmReview;
            detail_->setText(QStringLiteral("%1 · %2\n길이 %3 · 폭 %4\n면적 %5 · 표적지 겹침 %6\nLLM: %7 · 실제 대상: %8")
                .arg(displayId(object), object.label,
                    quantity(measurements, QStringLiteral("length"), QStringLiteral("mm")),
                    quantity(measurements, QStringLiteral("width_mean"), QStringLiteral("mm")),
                    measurements.contains(QStringLiteral("region_area")) ?
                        quantity(measurements, QStringLiteral("region_area"), QStringLiteral("mm2")) :
                        quantity(measurements, QStringLiteral("area"), QStringLiteral("mm2")),
                    object.flags.isEmpty() ? QStringLiteral("없음") : QStringLiteral("있음"),
                    llm.value(QStringLiteral("label")).toString(QStringLiteral("미검토")),
                    llm.value(QStringLiteral("actual")).toString(QStringLiteral("—"))));
            reason_->setPlainText(llm.value(QStringLiteral("reason")).toString());
            type_->setCurrentText(object.overrideType);
            note_->setText(object.note);
        } else {
            detail_->setText(QStringLiteral("선택한 손상 %1개").arg(selectedRows.size()));
            reason_->clear();
            type_->setCurrentText(QString());
            note_->clear();
        }
        QString state = object.reviewState;
        for (int selected : selectedRows)
            if (objectsData_[selected].reviewState != state) { state.clear(); break; }
        review_->setCurrentIndex(state.isEmpty() ? review_->findText(QStringLiteral("혼합")) :
            review_->findData(state));
        const QSignalBlocker blocker(scene_);
        for (auto *item : scene_->items()) {
            if (item->flags() & QGraphicsItem::ItemIsSelectable) {
                item->setSelected(selectedRows.contains(item->data(0).toInt()));
                if (item->isSelected()) item->setZValue(2);
                else item->setZValue(1);
            }
        }
        for (int selected : selectedRows) {
            const QRectF bounds = orientationTransform_.mapRect(boxPath(objectsData_[selected].bbox).boundingRect());
            if (!bounds.isValid() || bounds.isEmpty()) continue;
            QPen pen(QColor(220, 255, 44), 3, Qt::DashLine);
            pen.setCosmetic(true);
            auto *highlight = scene_->addRect(bounds, pen, Qt::NoBrush);
            highlight->setZValue(3);
            selectionHighlights_.append(highlight);
        }
    }
    void stageSelectedReview(bool verdictOnly) {
        const int imageRow = images_->currentRow();
        if (imageRow < 0 || imageRow >= imagesData_.size()) return;
        const auto selected = objects_->selectionModel()->selectedRows();
        if (selected.isEmpty() || (!verdictOnly && selected.size() != 1)) return;
        const QString state = review_->currentData().toString();
        if (verdictOnly && state.isEmpty()) return;
        const QString resultId = imagesData_[imageRow].resultId;
        bool refreshNeeded = false;
        const QStringList states{QString(), QString(), QStringLiteral("ACCEPTED"),
            QStringLiteral("HOLD"), QStringLiteral("REJECTED")};
        for (const auto &index : selected) {
            const int row = index.row();
            if (row < 0 || row >= objectsData_.size()) continue;
            auto &object = objectsData_[row];
            if (verdictOnly) object.reviewState = state;
            else {
                object.overrideType = type_->currentText().trimmed();
                object.note = note_->text();
            }
            drafts_[resultId].insert(object.id, {object.id, object.reviewState, object.overrideType, object.note});
            objects_->item(row, 1)->setText(object.overrideType.isEmpty() ? object.label : object.overrideType);
            objects_->item(row, 3)->setText(reviewLabel(object, true));
            const QColor color = colorFor(object);
            for (auto *item : scene_->items()) {
                auto *path = qgraphicsitem_cast<QGraphicsPathItem *>(item);
                if (!path || path->data(0).toInt() != row) continue;
                QPen pen(color, 2);
                pen.setCosmetic(true);
                path->setPen(pen);
                if (path->brush().style() != Qt::NoBrush)
                    path->setBrush(QColor(color.red(), color.green(), color.blue(), 45));
            }
            refreshNeeded |= (reviewFilter_->currentIndex() > 1 &&
                object.reviewState != states[reviewFilter_->currentIndex()]) ||
                (typeFilter_->currentText() != QStringLiteral("전체") &&
                 groupFor(object.overrideType.isEmpty() ? object.type : object.overrideType) != typeFilter_->currentText());
        }
        updateSaveButton();
        if (refreshNeeded) refreshObjects();
    }
    void updateSaveButton() {
        const int imageRow = images_->currentRow();
        const QString resultId = imageRow >= 0 && imageRow < imagesData_.size() ?
            imagesData_[imageRow].resultId : QString();
        const int changed = drafts_.value(resultId).size();
        const int pendingReview = imageRow >= 0 && imageRow < imagesData_.size() ?
            imagesData_[imageRow].pendingReviewCount : 0;
        for (int row = 0; row < imagesData_.size(); ++row) {
            const int drafts = drafts_.value(imagesData_[row].resultId).size();
            const int pending = imagesData_[row].pendingReviewCount;
            const bool needsReview = pending > 0 || drafts > 0;
            const QBrush background = needsReview ? QBrush(QColor(255, 246, 210)) : QBrush();
            const QString tip = needsReview ?
                QStringLiteral("검토 필요 %1건 · 변경 %2건").arg(pending).arg(drafts) : QString();
            for (int column = 0; column < images_->columnCount(); ++column) {
                auto *item = images_->item(row, column);
                if (!item) continue;
                item->setBackground(background);
                item->setToolTip(tip);
            }
        }
        saveReview_->setEnabled(pendingReview > 0 || changed > 0);
        saveReview_->setText(QStringLiteral("현재 영상 검토 저장"));
        saveReview_->setToolTip(QStringLiteral("검토 필요 %1건 · 변경 %2건").arg(pendingReview).arg(changed));
    }
    void saveImageReviews() {
        const int imageRow = images_->currentRow();
        if (imageRow < 0 || imageRow >= imagesData_.size()) return;
        const QString resultId = imagesData_[imageRow].resultId;
        const auto pending = drafts_.value(resultId);
        QString error;
        QList<DamageObject> all;
        if (!DamageStore::objects(projectDb_, resultId, &all, &error)) {
            QMessageBox::warning(this, QStringLiteral("검토 저장 실패"), error); return;
        }
        QList<DamageReview> reviews;
        for (const auto &object : all) {
            const auto draft = pending.value(object.id);
            if (draft.objectId.isEmpty() &&
                (object.reviewConfirmed || object.type.startsWith(QStringLiteral("target_")))) continue;
            reviews.append(draft.objectId.isEmpty() ? DamageReview{object.id, object.reviewState,
                object.overrideType, object.note} : draft);
        }
        if (reviews.isEmpty()) return;
        if (!DamageStore::setReviews(projectDb_, resultId, reviews, &error)) {
            QMessageBox::warning(this, QStringLiteral("검토 저장 실패"), error); return;
        }
        drafts_.remove(resultId);
        imagesData_[imageRow].pendingReviewCount = 0;
        refreshObjects();
        statusBar()->showMessage(QStringLiteral("현재 영상의 검토 %1건을 저장했습니다.").arg(reviews.size()), 4000);
    }
    void addManual() {
        const int imageRow = images_->currentRow();
        if (imageRow < 0 || imagesData_[imageRow].resultId.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("탐지 결과 필요"),
                QStringLiteral("탐지 결과가 등록된 영상을 선택하십시오.")); return;
        }
        bool accepted = false;
        const QString type = QInputDialog::getItem(this, QStringLiteral("손상 유형"),
            QStringLiteral("추가할 손상 유형"),
            {QStringLiteral("crack"), QStringLiteral("crack_region"), QStringLiteral("leakage_wetspot"),
             QStringLiteral("efflorescence"), QStringLiteral("spalling"), QStringLiteral("exposed_rebar")},
            0, true, &accepted).trimmed();
        if (!accepted || type.isEmpty()) return;
        view_->selectRegion([this, imageRow, type](const QRectF &area) {
            const auto &image = imagesData_[imageRow];
            const QRectF bounds = orientationTransform_.inverted().mapRect(area)
                .intersected(QRectF(0, 0, image.width, image.height));
            if (bounds.width() < 2 || bounds.height() < 2) return;
            QString error;
            if (!DamageStore::addObject(projectDb_, image.resultId, type,
                    QJsonArray{bounds.left(), bounds.top(), bounds.right(), bounds.bottom()}, &error)) {
                QMessageBox::warning(this, QStringLiteral("손상 추가 실패"), error); return;
            }
            imagesData_[imageRow].objectCount++;
            images_->item(imageRow, 1)->setText(QString::number(imagesData_[imageRow].objectCount));
            refreshObjects();
            statusBar()->showMessage(QStringLiteral("수동 손상을 추가했습니다."), 4000);
        });
        statusBar()->showMessage(QStringLiteral("영상에서 손상 영역을 드래그하십시오."));
    }
    void importResults() {
        const QString datasetId = dataset_->currentData().toString();
        if (projectDb_.isEmpty() || datasetId.isEmpty() || folder_->text().isEmpty()) {
            QMessageBox::information(this, QStringLiteral("입력 필요"),
                QStringLiteral("프로젝트, 데이터 세트, JSON 폴더를 선택하십시오.")); return;
        }
        if (!drafts_.isEmpty() && QMessageBox::question(this, QStringLiteral("미저장 검토"),
                QStringLiteral("JSON을 다시 가져오면 저장하지 않은 검토가 사라집니다. 계속하시겠습니까?"))
                != QMessageBox::Yes) return;
        if (std::any_of(imagesData_.begin(), imagesData_.end(), [](const DamageImage &image) {
                return !image.resultId.isEmpty();
            }) && QMessageBox::question(this, QStringLiteral("기존 결과 대체"),
                QStringLiteral("이미 등록된 결과가 있습니다. 새 JSON을 가져오면 해당 영상의 기존 검토 결과는 "
                               "이력으로 남고 새 결과가 활성화됩니다. 계속하시겠습니까?")) != QMessageBox::Yes)
            return;
        DamageImportReport report;
        QString error;
        if (!DamageStore::importFolder(projectDb_, datasetId, folder_->text(), &report, &error)) {
            QMessageBox::warning(this, QStringLiteral("가져오기 실패"), error); return;
        }
        drafts_.clear();
        refreshImages();
        QString summary = QStringLiteral("결과 %1장 · 객체 %2개를 등록했습니다.")
            .arg(report.images).arg(report.objects);
        if (!report.warnings.isEmpty())
            summary += QStringLiteral("\n\n건너뜀·경고 %1건:\n").arg(report.warnings.size()) +
                report.warnings.mid(0, 15).join(QLatin1Char('\n'));
        QMessageBox::information(this, QStringLiteral("가져오기 완료"), summary);
    }

    QString projectDb_, inspectionId_;
    QLabel *projectLabel_ = nullptr, *imageSummary_ = nullptr, *detail_ = nullptr;
    QPushButton *saveReview_ = nullptr;
    QLineEdit *folder_ = nullptr, *note_ = nullptr;
    QComboBox *dataset_ = nullptr, *typeFilter_ = nullptr, *reviewFilter_ = nullptr, *review_ = nullptr, *type_ = nullptr;
    QTableWidget *images_ = nullptr, *objects_ = nullptr;
    QPlainTextEdit *reason_ = nullptr;
    ImageView *view_ = nullptr;
    QGraphicsScene *scene_ = nullptr;
    QList<QGraphicsRectItem *> selectionHighlights_;
    QList<DamageImage> imagesData_;
    QList<DamageObject> objectsData_;
    QHash<QString, QHash<QString, DamageReview>> drafts_;
    QTransform orientationTransform_;
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QString project, inspection, capture;
    const auto args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--project") && i + 1 < args.size()) project = args[++i];
        else if (args[i] == QStringLiteral("--inspection") && i + 1 < args.size()) inspection = args[++i];
        else if (args[i] == QStringLiteral("--capture-ui") && i + 1 < args.size()) capture = args[++i];
    }
    DamageWindow window(project, inspection);
    window.show();
    if (!capture.isEmpty()) QTimer::singleShot(1600, &window, [&] {
        window.grab().save(capture);
        app.quit();
    });
    return app.exec();
}
