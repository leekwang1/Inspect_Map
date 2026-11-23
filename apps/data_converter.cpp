#include "conversion_core.h"
#include "image_correction.h"
#include <algorithm>
#include "lidar_preview.h"
#include "project_store.h"

#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QCheckBox>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMessageBox>
#include <QPixmap>
#include <QSignalBlocker>
#include <QStyle>
#include <QThread>
#include <QPushButton>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <memory>

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

class ImageView : public QGraphicsView {
public:
    explicit ImageView(QWidget *parent = nullptr) : QGraphicsView(parent), scene_(this) {
        setScene(&scene_);
        item_ = scene_.addPixmap({});
        setBackgroundBrush(QColor(QStringLiteral("#202e35")));
        setRenderHint(QPainter::SmoothPixmapTransform);
        setDragMode(QGraphicsView::ScrollHandDrag);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setResizeAnchor(QGraphicsView::AnchorViewCenter);
    }

    void loadImage(const QString &path) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QPixmap pixmap = QPixmap::fromImageReader(&reader);
        item_->setPixmap(pixmap);
        scene_.setSceneRect(item_->boundingRect());
        fitImage();
    }

    void fitImage() {
        fitMode_ = true;
        resetTransform();
        if (!item_->pixmap().isNull()) fitInView(item_, Qt::KeepAspectRatio);
    }

    void actualSize() {
        if (item_->pixmap().isNull()) return;
        fitMode_ = false;
        resetTransform();
        centerOn(item_);
    }

    void zoom(qreal factor) {
        if (item_->pixmap().isNull()) return;
        const qreal next = transform().m11() * factor;
        if (next < 0.03 || next > 16.0) return;
        fitMode_ = false;
        scale(factor, factor);
    }

protected:
    void resizeEvent(QResizeEvent *event) override {
        QGraphicsView::resizeEvent(event);
        if (fitMode_) fitImage();
    }

    void wheelEvent(QWheelEvent *event) override {
        zoom(event->angleDelta().y() > 0 ? 1.2 : 1.0 / 1.2);
        event->accept();
    }

private:
    QGraphicsScene scene_;
    QGraphicsPixmapItem *item_ = nullptr;
    bool fitMode_ = true;
};

class ConverterWindow : public QMainWindow {
public:
    ConverterWindow(const QString &projectPath, const QString &inspectionId)
        : inspectionId_(inspectionId) {
        setWindowTitle(QStringLiteral("InspectMap | 데이터 변환기"));
        resize(1280, 800);
        setMinimumSize(900, 600);
        setStyleSheet(QStringLiteral(R"(
            QWidget { background:#f6f8f8; color:#203039; font-size:13px; }
            QWidget#header { background:#263b4e; }
            QWidget#header QLabel { color:white; background:transparent; font-size:19px; font-weight:700; }
            QLabel#section { font-size:16px; font-weight:600; }
            QLabel#muted { color:#66777e; }
            QPushButton { background:white; border:1px solid #c8d4d6; border-radius:4px; padding:8px 14px; }
            QPushButton:hover { border-color:#14796f; }
            QPushButton#primary { background:#14796f; color:white; border-color:#14796f; }
            QPushButton:disabled { background:#e9eeee; color:#88989c; }
            QLineEdit,QTableWidget,QListWidget,QComboBox { background:white; border:1px solid #c8d4d6; }
            QTableWidget { selection-background-color:#dcece8; selection-color:#173b37; }
            QTableWidget::item:selected { background:#dcece8; color:#173b37; border:0; border-radius:0; }
            QTableWidget::item:focus { outline:none; border:0; }
            QLineEdit { padding:7px; }
            QHeaderView::section { background:#e9eeee; padding:7px; border:0; }
            QTabWidget::pane { border:1px solid #c8d4d6; background:white; }
            QTabBar::tab { background:#e8eeef; color:#4d626a; border:1px solid #c8d4d6;
                           border-bottom:3px solid #c8d4d6; padding:10px 18px; }
            QTabBar::tab:selected { background:white; color:#0d6159; font-weight:700;
                                    border-bottom:3px solid #14796f; }
        )"));
        buildUi();
        if (!projectPath.isEmpty()) openProject(projectPath);
    }

    void showLidarTab() { if (tabs_->isTabVisible(cloudTabIndex_)) tabs_->setCurrentIndex(cloudTabIndex_); }

private:
    void closeEvent(QCloseEvent *event) override {
        if (correctionRunning_) {
            event->ignore();
            status_->setText(QStringLiteral("이미지 보정이 끝난 뒤 창을 닫을 수 있습니다."));
        } else {
            if (cloudThread_) cloudThread_->wait();
            QMainWindow::closeEvent(event);
        }
    }

    void buildUi() {
        auto *root = new QWidget;
        auto *layout = new QVBoxLayout(root);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto *header = new QWidget;
        header->setObjectName(QStringLiteral("header"));
        auto *headerRow = new QHBoxLayout(header);
        headerRow->addWidget(new QLabel(QStringLiteral("InspectMap · 데이터 변환기")));
        layout->addWidget(header);

        auto *sourceBar = new QHBoxLayout;
        sourceBar->setContentsMargins(16, 12, 16, 8);
        projectLabel_ = new QLabel(QStringLiteral("프로젝트 미선택"));
        projectLabel_->setObjectName(QStringLiteral("muted"));
        auto *open = new QPushButton(QStringLiteral("프로젝트 열기"));
        sourceBar->addWidget(projectLabel_, 1);
        sourceBar->addWidget(open);
        layout->addLayout(sourceBar);
        connect(open, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("project.sqlite 선택"), {}, QStringLiteral("project.sqlite (project.sqlite)"));
            if (!path.isEmpty()) openProject(path);
        });

        auto *inputBar = new QHBoxLayout;
        inputBar->setContentsMargins(16, 0, 16, 10);
        sourcePath_ = new QLineEdit;
        sourcePath_->setPlaceholderText(QStringLiteral("드론 원본 폴더"));
        auto *browseSource = new QPushButton(QStringLiteral("원본 폴더"));
        adapter_ = new QComboBox;
        adapter_->addItem(QStringLiteral("자동 판별"), QStringLiteral("AUTO"));
        adapter_->addItem(QStringLiteral("Elios Inspector 5"), QStringLiteral("ELIOS_INSPECTOR5"));
        adapter_->addItem(QStringLiteral("이미지 폴더 (DJI 포함)"), QStringLiteral("IMAGE_FOLDER"));
        auto *scanButton = new QPushButton(QStringLiteral("스캔"));
        importButton_ = new QPushButton(QStringLiteral("프로젝트에 등록"));
        importButton_->setObjectName(QStringLiteral("primary"));
        importButton_->setEnabled(false);
        inputBar->addWidget(sourcePath_, 1);
        inputBar->addWidget(browseSource);
        inputBar->addWidget(adapter_);
        inputBar->addWidget(scanButton);
        inputBar->addWidget(importButton_);
        layout->addLayout(inputBar);
        connect(browseSource, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getExistingDirectory(this, QStringLiteral("드론 원본 폴더"));
            if (!path.isEmpty()) sourcePath_->setText(path);
        });
        connect(scanButton, &QPushButton::clicked, this, [this] { scanSource(); });
        connect(importButton_, &QPushButton::clicked, this, [this] { importSource(); });

        auto *splitter = new QSplitter(Qt::Horizontal);
        auto *left = new QWidget;
        auto *leftLayout = new QVBoxLayout(left);
        leftLayout->addWidget(section(QStringLiteral("데이터 세트")));
        datasets_ = new QListWidget;
        leftLayout->addWidget(datasets_, 1);
        restoreNamesButton_ = new QPushButton(QStringLiteral("기존 파일명 복원"));
        restoreNamesButton_->setEnabled(false);
        leftLayout->addWidget(restoreNamesButton_);
        leftLayout->addWidget(section(QStringLiteral("이미지 보정")));
        profilePath_ = new QLineEdit;
        profilePath_->setPlaceholderText(QStringLiteral("K/D/P JSON 프로필"));
        auto *browseProfile = new QPushButton(QStringLiteral("프로필 선택"));
        leftLayout->addWidget(profilePath_);
        leftLayout->addWidget(browseProfile);
        profileStatus_ = new QLabel(QStringLiteral("보정 프로필 없음"));
        profileStatus_->setObjectName(QStringLiteral("muted"));
        profileStatus_->setWordWrap(true);
        leftLayout->addWidget(profileStatus_);
        correctButton_ = new QPushButton(QStringLiteral("선택 데이터 세트 이미지 보정"));
        correctButton_->setObjectName(QStringLiteral("primary"));
        correctButton_->setEnabled(false);
        leftLayout->addWidget(correctButton_);
        splitter->addWidget(left);
        connect(browseProfile, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Calibration JSON"), {}, QStringLiteral("JSON (*.json)"));
            if (!path.isEmpty()) profilePath_->setText(path);
        });
        connect(profilePath_, &QLineEdit::textChanged, this, [this] { updateCorrectionUi(); });
        connect(correctButton_, &QPushButton::clicked, this, [this] { correctImages(); });
        connect(restoreNamesButton_, &QPushButton::clicked, this, [this] {
            if (datasetId_.isEmpty() || correctionRunning_) return;
            if (QMessageBox::question(this, QStringLiteral("기존 파일명 복원"),
                QStringLiteral("UUID 이름으로 저장된 이미지를 원본 파일명으로 복사하고 DB 경로를 갱신합니다. 기존 UUID 파일은 남겨둡니다. 진행하시겠습니까?")) != QMessageBox::Yes) return;
            QApplication::setOverrideCursor(Qt::WaitCursor);
            int restored = 0;
            QString error;
            const bool ok = ConversionCore::restoreReadableNames(projectDb_, datasetId_, &restored, &error);
            QApplication::restoreOverrideCursor();
            if (!ok) { QMessageBox::warning(this, QStringLiteral("복원 실패"), error); return; }
            selectDataset(datasets_->currentRow());
            QMessageBox::information(this, QStringLiteral("복원 완료"), QStringLiteral("%1장의 파일명을 복원했습니다.").arg(restored));
        });
        connect(datasets_, &QListWidget::currentRowChanged, this, [this](int row) { selectDataset(row); });

        tabs_ = new QTabWidget;
        auto *imagePage = new QWidget;
        auto *imageLayout = new QVBoxLayout(imagePage);
        summary_ = new QLabel(QStringLiteral("원본 폴더를 스캔하세요."));
        summary_->setWordWrap(true);
        imageLayout->addWidget(summary_);
        auto *reviewSplit = new QSplitter(Qt::Horizontal);
        imageTable_ = new QTableWidget(0, 4);
        imageTable_->setHorizontalHeaderLabels({QStringLiteral("순서"), QStringLiteral("영상"), QStringLiteral("pose"), QStringLiteral("보정")});
        imageTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        imageTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        imageTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        imageTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        imageTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        imageTable_->setSelectionMode(QAbstractItemView::SingleSelection);
        imageTable_->setShowGrid(false);
        imageTable_->setItemDelegate(new RowSelectionDelegate(imageTable_));
        imageTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        imageTable_->verticalHeader()->hide();
        reviewSplit->addWidget(imageTable_);

        auto *viewer = new QWidget;
        auto *viewerLayout = new QVBoxLayout(viewer);
        auto *navigation = new QHBoxLayout;
        auto *toolbar = new QHBoxLayout;
        auto *previous = new QPushButton;
        previous->setIcon(style()->standardIcon(QStyle::SP_ArrowLeft));
        previous->setToolTip(QStringLiteral("이전 영상"));
        auto *next = new QPushButton;
        next->setIcon(style()->standardIcon(QStyle::SP_ArrowRight));
        next->setToolTip(QStringLiteral("다음 영상"));
        previous->setFixedWidth(36);
        next->setFixedWidth(36);
        previous_ = previous;
        next_ = next;
        navigation->addWidget(previous);
        navigation->addWidget(next);
        imageName_ = new QLabel;
        imageName_->setObjectName(QStringLiteral("muted"));
        imageName_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        navigation->addWidget(imageName_, 1);
        auto *modes = new QButtonGroup(this);
        modes->setExclusive(true);
        for (const auto &[label, mode] : {std::pair{QStringLiteral("원본"), 0},
                                          std::pair{QStringLiteral("보정"), 1},
                                          std::pair{QStringLiteral("비교"), 2}}) {
            auto *button = new QPushButton(label);
            button->setCheckable(true);
            button->setProperty("viewMode", mode);
            modes->addButton(button);
            toolbar->addWidget(button);
            if (mode == 0) rawMode_ = button;
            else if (mode == 1) correctedMode_ = button;
            else compareMode_ = button;
            connect(button, &QPushButton::clicked, this, [this, mode] { setViewMode(mode); });
        }
        rawMode_->setChecked(true);
        auto *zoomOut = new QPushButton(QStringLiteral("−"));
        auto *zoomIn = new QPushButton(QStringLiteral("+"));
        auto *actual = new QPushButton(QStringLiteral("1:1"));
        auto *fit = new QPushButton(QStringLiteral("맞춤"));
        zoomOut->setToolTip(QStringLiteral("축소"));
        zoomIn->setToolTip(QStringLiteral("확대"));
        actual->setToolTip(QStringLiteral("원본 픽셀 크기"));
        fit->setToolTip(QStringLiteral("화면에 맞춤"));
        toolbar->addWidget(zoomOut);
        toolbar->addWidget(zoomIn);
        toolbar->addWidget(actual);
        toolbar->addWidget(fit);
        toolbar->addStretch(1);
        viewerLayout->addLayout(navigation);
        viewerLayout->addLayout(toolbar);
        imageViews_ = new QSplitter(Qt::Horizontal);
        rawView_ = new ImageView;
        correctedView_ = new ImageView;
        imageViews_->addWidget(rawView_);
        imageViews_->addWidget(correctedView_);
        imageViews_->setChildrenCollapsible(false);
        viewerLayout->addWidget(imageViews_, 1);
        reviewSplit->addWidget(viewer);
        reviewSplit->setChildrenCollapsible(false);
        reviewSplit->setSizes({280, 700});
        imageLayout->addWidget(reviewSplit, 1);
        tabs_->addTab(imagePage, QStringLiteral("영상 검토"));
        setViewMode(0);
        connect(imageTable_, &QTableWidget::currentCellChanged, this,
            [this](int row) { displayImage(row); });
        connect(previous, &QPushButton::clicked, this, [this] { selectImage(imageTable_->currentRow() - 1); });
        connect(next, &QPushButton::clicked, this, [this] { selectImage(imageTable_->currentRow() + 1); });
        connect(zoomOut, &QPushButton::clicked, this, [this] { zoomVisible(1.0 / 1.2); });
        connect(zoomIn, &QPushButton::clicked, this, [this] { zoomVisible(1.2); });
        connect(actual, &QPushButton::clicked, this, [this] { forVisibleViews([](ImageView *view) { view->actualSize(); }); });
        connect(fit, &QPushButton::clicked, this, [this] { forVisibleViews([](ImageView *view) { view->fitImage(); }); });

        auto *cloudPage = new QWidget;
        auto *cloudLayout = new QVBoxLayout(cloudPage);
        auto *cloudSplit = new QSplitter(Qt::Horizontal);
        cloudTable_ = new QTableWidget(0, 3);
        cloudTable_->setHorizontalHeaderLabels({QStringLiteral("유형"), QStringLiteral("크기"), QStringLiteral("파일")});
        cloudTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
        cloudTable_->setSelectionMode(QAbstractItemView::SingleSelection);
        cloudTable_->setShowGrid(false);
        cloudTable_->setItemDelegate(new RowSelectionDelegate(cloudTable_));
        cloudTable_->setMinimumWidth(260);
        cloudTable_->setMaximumWidth(360);
        cloudTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        cloudTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        cloudTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        cloudTable_->verticalHeader()->hide();
        cloudSplit->addWidget(cloudTable_);
        auto *cloudViewer = new QWidget;
        auto *cloudViewerLayout = new QVBoxLayout(cloudViewer);
        auto *cloudControls = new QHBoxLayout;
        cloudImage_ = new QComboBox;
        cloudImage_->setMinimumWidth(180);
        cloudControls->addWidget(new QLabel(QStringLiteral("영상")));
        cloudControls->addWidget(cloudImage_, 1);
        auto *showCameras = new QCheckBox(QStringLiteral("촬영 위치"));
        auto *showFrustum = new QCheckBox(QStringLiteral("프러스텀"));
        showCameras->setChecked(true);
        showFrustum->setChecked(true);
        auto *resetCloudView = new QPushButton(QStringLiteral("화면 맞춤"));
        cloudControls->addWidget(showCameras);
        cloudControls->addWidget(showFrustum);
        cloudControls->addWidget(resetCloudView);
        cloudViewerLayout->addLayout(cloudControls);
        cloudInfo_ = new QLabel(QStringLiteral("LiDAR 파일을 선택하세요."));
        cloudInfo_->setObjectName(QStringLiteral("muted"));
        cloudViewerLayout->addWidget(cloudInfo_);
        cloudPreview_ = new LidarPreview;
        cloudViewerLayout->addWidget(cloudPreview_, 1);
        cloudSplit->addWidget(cloudViewer);
        cloudSplit->setChildrenCollapsible(false);
        cloudSplit->setSizes({300, 700});
        cloudLayout->addWidget(cloudSplit, 1);
        cloudTabIndex_ = tabs_->addTab(cloudPage, QStringLiteral("LiDAR 입력"));
        tabs_->setTabVisible(cloudTabIndex_, false);
        connect(cloudTable_, &QTableWidget::currentCellChanged, this, [this](int row) { loadCloud(row); });
        connect(cloudImage_, &QComboBox::currentIndexChanged, this, [this](int row) { selectImage(row); });
        connect(showCameras, &QCheckBox::toggled, cloudPreview_, &LidarPreview::setShowCameras);
        connect(showFrustum, &QCheckBox::toggled, cloudPreview_, &LidarPreview::setShowFrustum);
        connect(resetCloudView, &QPushButton::clicked, cloudPreview_, &LidarPreview::resetView);
        splitter->addWidget(tabs_);
        splitter->setSizes({300, 930});
        layout->addWidget(splitter, 1);

        status_ = new QLabel(QStringLiteral("프로젝트와 원본 데이터를 선택하세요."));
        status_->setObjectName(QStringLiteral("muted"));
        status_->setWordWrap(true);
        status_->setContentsMargins(16, 8, 16, 8);
        layout->addWidget(status_);
        setCentralWidget(root);
    }

    QLabel *section(const QString &text) {
        auto *label = new QLabel(text);
        label->setObjectName(QStringLiteral("section"));
        return label;
    }

    void openProject(const QString &path) {
        if (correctionRunning_) {
            QMessageBox::information(this, QStringLiteral("보정 진행 중"), QStringLiteral("현재 보정 작업이 끝난 뒤 프로젝트를 변경할 수 있습니다."));
            return;
        }
        ProjectInfo info;
        QString error;
        if (!ProjectStore::read(path, &info, &error)) {
            QMessageBox::warning(this, QStringLiteral("프로젝트 열기 실패"), error);
            return;
        }
        projectDb_ = QFileInfo(path).absoluteFilePath();
        projectLabel_->setText(info.name + QStringLiteral(" · ") + projectDb_);
        reloadDatasets();
    }

    void scanSource() {
        if (correctionRunning_) return;
        QString error;
        if (!ConversionCore::scan(sourcePath_->text().trimmed(), adapter_->currentData().toString(), &scan_, &error)) {
            QMessageBox::warning(this, QStringLiteral("스캔 실패"), error);
            importButton_->setEnabled(false);
            return;
        }
        displayed_ = {};
        restoreNamesButton_->setEnabled(false);
        for (const auto &item : scan_.images)
            displayed_.append({{}, item.sourceId, item.sourcePath, {}, item.width, item.height,
                               !item.poseJson.isEmpty() && item.poseJson != QStringLiteral("{}"), item.poseJson,
                               QFileInfo(item.sourcePath).fileName(), item.captureTime});
        fillImages(false);
        fillClouds(scan_.clouds);
        int poseCount = 0;
        for (const auto &image : scan_.images)
            if (!image.poseJson.isEmpty() && image.poseJson != QStringLiteral("{}")) ++poseCount;
        tabs_->setTabVisible(cloudTabIndex_, !scan_.clouds.isEmpty());
        summary_->setText(QStringLiteral("%1 · 이미지 %2장 · 초기 pose %3장 · 점군 %4개 · 경고 %5건")
            .arg(scan_.adapter).arg(scan_.images.size()).arg(poseCount)
            .arg(scan_.clouds.size()).arg(scan_.warnings.size()));
        importButton_->setEnabled(!projectDb_.isEmpty());
        datasets_->setCurrentRow(-1);
        profilePath_->clear();
        datasetId_.clear();
        updateCorrectionUi();
        selectImage(0);
        status_->setText(scan_.warnings.isEmpty() ? QStringLiteral("스캔 완료 · 프로젝트에 등록하기 전 원본은 변경되지 않았습니다.")
                                            : scan_.warnings.join(QStringLiteral(" | ")));
    }

    void importSource() {
        if (projectDb_.isEmpty() || scan_.images.isEmpty()) return;
        QString id, error;
        if (!ConversionCore::importDataset(projectDb_, inspectionId_, scan_, &id, &error)) {
            QMessageBox::warning(this, QStringLiteral("프로젝트 등록 실패"), error);
            return;
        }
        reloadDatasets(id);
        const QString message = QStringLiteral("프로젝트 등록이 완료되었습니다.\n원본 이미지는 프로젝트의 source/images에 저장되었고, LAS는 원본 경로로 연결됩니다.");
        status_->setText(QStringLiteral("프로젝트 등록 완료"));
        QMessageBox::information(this, QStringLiteral("등록 완료"), message);
    }

    void reloadDatasets(const QString &selectId = {}) {
        QString error;
        if (!ConversionCore::datasets(projectDb_, &datasetList_, &error)) {
            QMessageBox::warning(this, QStringLiteral("데이터 세트 조회 실패"), error);
            return;
        }
        datasets_->blockSignals(true);
        datasets_->clear();
        int selected = -1;
        for (int i = 0; i < datasetList_.size(); ++i) {
            datasets_->addItem(datasetList_[i].name + QStringLiteral(" · %1장").arg(datasetList_[i].imageCount));
            if (datasetList_[i].id == selectId) selected = i;
        }
        datasets_->blockSignals(false);
        if (selected < 0 && !datasetList_.isEmpty()) selected = 0;
        if (selected >= 0) datasets_->setCurrentRow(selected);
        else {
            restoreNamesButton_->setEnabled(false);
            datasetId_.clear();
            displayed_.clear();
            fillImages(true);
            fillClouds({});
            tabs_->setTabVisible(cloudTabIndex_, false);
            profilePath_->clear();
            updateCorrectionUi();
            displayImage(-1);
        }
    }

    void selectDataset(int row) {
        if (row < 0 || row >= datasetList_.size()) return;
        datasetId_ = datasetList_[row].id;
        QString error;
        if (!ConversionCore::images(projectDb_, datasetId_, &displayed_, &error)) {
            QMessageBox::warning(this, QStringLiteral("이미지 조회 실패"), error);
            return;
        }
        restoreNamesButton_->setEnabled(std::any_of(displayed_.cbegin(), displayed_.cend(),
            [](const ImportedImage &image) { return image.displayName.isEmpty(); }));
        QList<SourceCloud> clouds;
        if (!ConversionCore::clouds(projectDb_, datasetId_, &clouds, &error)) {
            QMessageBox::warning(this, QStringLiteral("LiDAR 조회 실패"), error);
            return;
        }
        fillImages(true);
        fillClouds(clouds);
        tabs_->setTabVisible(cloudTabIndex_, !clouds.isEmpty());
        const auto &dataset = datasetList_[row];
        const QString adapterName = dataset.adapter == QStringLiteral("ELIOS_INSPECTOR5") ?
            QStringLiteral("Elios Inspector 5") : QStringLiteral("이미지 폴더");
        summary_->setText(QStringLiteral("%1 · %2 · 이미지 %3장 · 초기 pose %4장 · 점군 %5개 · 보정 %6장")
            .arg(dataset.name, adapterName).arg(dataset.imageCount).arg(dataset.poseCount)
            .arg(dataset.cloudCount).arg(dataset.correctedCount));
        QString activeProfile;
        if (ConversionCore::activeProfile(projectDb_, datasetId_, &activeProfile, &error))
            profilePath_->setText(activeProfile);
        else profilePath_->clear();
        updateCorrectionUi();
        selectImage(0);
    }

    void updateCorrectionUi() {
        if (!profileStatus_ || !correctButton_) return;
        const QString path = profilePath_->text().trimmed();
        CorrectionProfileInfo profile;
        QString error;
        const bool valid = !path.isEmpty() && ImageCorrection::inspectProfile(path, &profile, &error);
        profileStatus_->setText(path.isEmpty() ? QStringLiteral("보정 프로필 없음") :
            valid ? QStringLiteral("%1 · fisheye · %2×%3")
                .arg(profile.cameraModel.isEmpty() ? QStringLiteral("카메라 미지정") : profile.cameraModel)
                .arg(profile.width).arg(profile.height) : QStringLiteral("프로필 오류: ") + error);
        correctButton_->setEnabled(valid && !datasetId_.isEmpty() && !displayed_.isEmpty() && !correctionRunning_);
    }

    void fillImages(bool imported) {
        const QSignalBlocker blocker(imageTable_);
        const QSignalBlocker cloudBlocker(cloudImage_);
        imageTable_->clearSelection();
        imageTable_->setRowCount(displayed_.size());
        cloudImage_->clear();
        QVector<PreviewCamera> cameras;
        cameras.reserve(displayed_.size());
        for (int row = 0; row < displayed_.size(); ++row) {
            const auto &image = displayed_[row];
            const QString name = image.displayName.isEmpty() ? QFileInfo(image.rawPath).fileName() : image.displayName;
            cloudImage_->addItem(name.isEmpty() ? image.sourceId : name);
            cameras.append(parsePreviewCamera(image.poseJson));
            const QStringList values{QString::number(row + 1), name.isEmpty() ? image.sourceId : name,
                image.hasPose ? QStringLiteral("있음") : QStringLiteral("없음"),
                image.correctedPath.isEmpty() ? QStringLiteral("없음") : QStringLiteral("완료")};
            for (int col = 0; col < values.size(); ++col) {
                auto *item = new QTableWidgetItem(values[col]);
                if (col == 1) item->setToolTip(QStringLiteral("%1\n%2×%3\n%4")
                    .arg(image.sourceId).arg(image.width).arg(image.height)
                    .arg(imported ? QStringLiteral("프로젝트 원본") : QStringLiteral("스캔 원본")));
                imageTable_->setItem(row, col, item);
            }
        }
        cloudPreview_->setCameras(std::move(cameras));
    }

    void fillClouds(const QList<SourceCloud> &clouds) {
        const QSignalBlocker blocker(cloudTable_);
        cloudList_ = clouds;
        cloudTable_->setRowCount(clouds.size());
        for (int row = 0; row < clouds.size(); ++row) {
            cloudTable_->setItem(row, 0, new QTableWidgetItem(clouds[row].scope));
            const QFileInfo file(clouds[row].sourcePath);
            cloudTable_->setItem(row, 1, new QTableWidgetItem(QStringLiteral("%1 MB")
                .arg(file.size() / 1048576.0, 0, 'f', 1)));
            auto *name = new QTableWidgetItem(file.fileName());
            name->setToolTip(clouds[row].sourcePath);
            cloudTable_->setItem(row, 2, name);
        }
        if (!clouds.isEmpty()) {
            int preferred = 0;
            for (int row = 1; row < clouds.size(); ++row)
                if (QFileInfo(clouds[row].sourcePath).size() > QFileInfo(clouds[preferred].sourcePath).size())
                    preferred = row;
            cloudTable_->setCurrentCell(preferred, 0);
            loadCloud(preferred);
        } else {
            pendingCloudPath_.clear();
            cloudPreview_->setCloud({});
            cloudInfo_->setText(QStringLiteral("LiDAR 파일 없음"));
        }
    }

    void loadCloud(int row) {
        if (row < 0 || row >= cloudList_.size()) return;
        const auto &cloud = cloudList_[row];
        pendingCloudPath_ = cloud.sourcePath;
        cloudInfo_->setText(QStringLiteral("%1 · %2 · 미리보기 불러오는 중")
            .arg(cloud.scope, QFileInfo(cloud.sourcePath).fileName()));
        if (cloudThread_) return;
        startCloudLoad();
    }

    void startCloudLoad() {
        if (pendingCloudPath_.isEmpty()) return;
        const QString path = pendingCloudPath_;
        const auto result = std::make_shared<CloudPreview>();
        cloudThread_ = QThread::create([path, result] { *result = readLasPreview(path); });
        connect(cloudThread_, &QThread::finished, this, [this, path, result] {
            cloudThread_ = nullptr;
            if (path != pendingCloudPath_) { startCloudLoad(); return; }
            cloudPreview_->setCloud(*result);
            cloudInfo_->setText(result->error.isEmpty() ?
                QStringLiteral("%1 · 전체 %2점 / 표시 %3점 · 신호강도 %4-%5 (2~98%) · 촬영 위치는 초기 pose")
                    .arg(QFileInfo(path).fileName()).arg(result->totalPoints).arg(result->points.size())
                    .arg(result->intensityLow).arg(result->intensityHigh) :
                result->error);
        });
        connect(cloudThread_, &QThread::finished, cloudThread_, &QObject::deleteLater);
        cloudThread_->start();
    }

    void selectImage(int row) {
        if (row < 0 || row >= displayed_.size()) return;
        const bool sameRow = imageTable_->currentRow() == row;
        imageTable_->setCurrentCell(row, 0);
        if (sameRow) displayImage(row);
    }

    void displayImage(int row) {
        const bool valid = row >= 0 && row < displayed_.size();
        previous_->setEnabled(valid && row > 0);
        next_->setEnabled(valid && row + 1 < displayed_.size());
        if (!valid) {
            imageName_->clear();
            rawView_->loadImage({});
            correctedView_->loadImage({});
            cloudPreview_->setSelectedCamera(-1);
            correctedMode_->setEnabled(false);
            compareMode_->setEnabled(false);
            setViewMode(0);
            return;
        }
        const auto &image = displayed_[row];
        const auto raw = QFileInfo(image.rawPath).isAbsolute() ? image.rawPath :
            QFileInfo(projectDb_).absoluteDir().filePath(image.rawPath);
        rawView_->loadImage(raw);
        const auto corrected = image.correctedPath.isEmpty() ? QString() :
            QFileInfo(projectDb_).absoluteDir().filePath(image.correctedPath);
        correctedView_->loadImage(corrected);
        imageName_->setText(QStringLiteral("%1 / %2 · %3")
            .arg(row + 1).arg(displayed_.size()).arg(QFileInfo(raw).fileName()));
        imageName_->setToolTip(raw);
        cloudPreview_->setSelectedCamera(row);
        if (cloudImage_->currentIndex() != row) {
            const QSignalBlocker blocker(cloudImage_);
            cloudImage_->setCurrentIndex(row);
        }
        correctedMode_->setEnabled(!corrected.isEmpty());
        compareMode_->setEnabled(!corrected.isEmpty());
        if (corrected.isEmpty() && viewMode_ != 0) setViewMode(0);
    }

    void setViewMode(int mode) {
        viewMode_ = mode;
        rawMode_->setChecked(mode == 0);
        correctedMode_->setChecked(mode == 1);
        compareMode_->setChecked(mode == 2);
        rawView_->setVisible(mode != 1);
        correctedView_->setVisible(mode != 0);
        if (mode == 2) imageViews_->setSizes({1, 1});
        QTimer::singleShot(0, this, [this] {
            forVisibleViews([](ImageView *view) { view->fitImage(); });
        });
    }

    template <typename F>
    void forVisibleViews(F action) {
        if (rawView_->isVisible()) action(rawView_);
        if (correctedView_->isVisible()) action(correctedView_);
    }

    void zoomVisible(qreal factor) {
        forVisibleViews([factor](ImageView *view) { view->zoom(factor); });
    }

    void correctImages() {
        if (datasetId_.isEmpty() || correctionRunning_) return;
        CorrectionProfileInfo profileInfo;
        QString profileError;
        const QString selectedProfile = profilePath_->text().trimmed();
        if (!ImageCorrection::inspectProfile(selectedProfile, &profileInfo, &profileError)) {
            QMessageBox::warning(this, QStringLiteral("보정 불가"), profileError);
            return;
        }
        correctionRunning_ = true;
        correctButton_->setEnabled(false);
        datasets_->setEnabled(false);
        status_->setText(QStringLiteral("이미지 보정 중 · %1장").arg(displayed_.size()));
        const QString currentDataset = datasetId_;
        const QString currentProject = projectDb_;
        QThread *worker = QThread::create([this, currentProject, currentDataset, selectedProfile] {
            CorrectionResult result;
            QString error;
            const bool ok = ImageCorrection::run(currentProject, currentDataset, selectedProfile, &result,
                [this](int done, int total) {
                    QMetaObject::invokeMethod(this, [this, done, total] {
                        status_->setText(QStringLiteral("이미지 보정 중 · %1 / %2장").arg(done).arg(total));
                    }, Qt::QueuedConnection);
                }, &error);
            int registered = 0;
            if (ok && !ConversionCore::registerCorrections(currentProject, currentDataset, selectedProfile, &registered, &error))
                error = QStringLiteral("결과 DB 등록 실패: ") + error;
            QMetaObject::invokeMethod(this, [this, ok, result, error, registered] {
                correctionRunning_ = false;
                datasets_->setEnabled(true);
                selectDataset(datasets_->currentRow());
                const QString outcome = !error.isEmpty() ? error : result.failed ?
                    QStringLiteral("%1장 실패: %2").arg(result.failed).arg(result.errors.join(QStringLiteral(" | ")).left(500)) :
                    ok ? QStringLiteral("완료") : QStringLiteral("실패");
                status_->setText(QStringLiteral("보정 결과 %1장 등록 · %2").arg(registered).arg(outcome));
            }, Qt::QueuedConnection);
        });
        connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
    }

    QString projectDb_;
    QString inspectionId_;
    QString datasetId_;
    ScanResult scan_;
    QList<ImportedDataset> datasetList_;
    QList<ImportedImage> displayed_;
    QLineEdit *sourcePath_ = nullptr;
    QLineEdit *profilePath_ = nullptr;
    QComboBox *adapter_ = nullptr;
    QListWidget *datasets_ = nullptr;
    QTableWidget *imageTable_ = nullptr;
    QTableWidget *cloudTable_ = nullptr;
    QComboBox *cloudImage_ = nullptr;
    QLabel *cloudInfo_ = nullptr;
    LidarPreview *cloudPreview_ = nullptr;
    QList<SourceCloud> cloudList_;
    QString pendingCloudPath_;
    QThread *cloudThread_ = nullptr;
    QLabel *projectLabel_ = nullptr;
    QLabel *summary_ = nullptr;
    QLabel *imageName_ = nullptr;
    ImageView *rawView_ = nullptr;
    ImageView *correctedView_ = nullptr;
    QSplitter *imageViews_ = nullptr;
    QPushButton *previous_ = nullptr;
    QPushButton *next_ = nullptr;
    QPushButton *rawMode_ = nullptr;
    QPushButton *correctedMode_ = nullptr;
    QPushButton *compareMode_ = nullptr;
    QLabel *status_ = nullptr;
    QLabel *profileStatus_ = nullptr;
    QPushButton *importButton_ = nullptr;
    QPushButton *correctButton_ = nullptr;
    QPushButton *restoreNamesButton_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    int cloudTabIndex_ = -1;
    int viewMode_ = 0;
    bool correctionRunning_ = false;
};
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QString project, inspection, capture;
    bool captureLidar = false;
    const auto args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--project") && i + 1 < args.size()) project = args[++i];
        else if (args[i] == QStringLiteral("--inspection") && i + 1 < args.size()) inspection = args[++i];
        else if (args[i] == QStringLiteral("--capture-ui") && i + 1 < args.size()) capture = args[++i];
        else if (args[i] == QStringLiteral("--capture-lidar")) captureLidar = true;
    }
    ConverterWindow window(project, inspection);
    window.show();
    if (captureLidar) window.showLidarTab();
    if (!capture.isEmpty()) QTimer::singleShot(captureLidar ? 1800 : 100, &window, [&] {
        window.grab().save(capture);
        app.quit();
    });
    return app.exec();
}
