#include "project_store.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QSplitter>
#include <QTableWidget>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <iterator>

namespace {
struct ModuleDefinition { const char *id; const char *name; const char *program; };
constexpr ModuleDefinition modules[] = {
    {"project", "프로젝트·작업 상태", "ProjectManager"},
    {"conversion", "입력·변환", "DataConverter"},
    {"model_drawing", "모델·도면 입력", "RegistrationTool"},
    {"sfm", "SfM/MVS", "RegistrationTool"},
    {"registration", "정합", "RegistrationTool"},
    {"camera_pose", "카메라 위치·자세", "RegistrationTool"},
    {"visibility", "이미지 부재 판정", "InspectionMapEditor"},
    {"ai_damage", "AI 손상정보", "AIDamageManager"},
    {"projection", "손상 3D 투영", "InspectionMapEditor"},
    {"quantity", "손상 물량 계산", "InspectionMapEditor"},
    {"map", "외관조사망도·산출물", "InspectionMapEditor"}
};

struct WorkCounts {
    int datasets = 0;
    int images = 0;
    int corrected = 0;
    int clouds = 0;
    int registrations = 0;
    int alignedPoses = 0;
    int damageImages = 0;
    int damageObjects = 0;
    int damagePending = 0;
    int mapRuns = 0;
    int projectedDamages = 0;
    int projectionPending = 0;
    int mapParts = 0;
    int measuredDamages = 0;
};

WorkCounts readWorkCounts(const QString &projectDb, const QString &inspectionId) {
    WorkCounts result;
    const QString connection = QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        db.setDatabaseName(projectDb);
        if (db.open()) {
            const auto tables = db.tables();
            auto count = [&](const QString &statement) {
                QSqlQuery query(db);
                query.prepare(statement);
                const QString scope = inspectionId.isEmpty() ? QStringLiteral("") : inspectionId;
                query.addBindValue(scope);
                query.addBindValue(scope);
                return query.exec() && query.next() ? query.value(0).toInt() : 0;
            };
            if (tables.contains(QStringLiteral("dataset"))) {
                result.datasets = count(QStringLiteral("SELECT count(*) FROM dataset d "
                    "WHERE (?='' OR d.inspection_id=? OR d.inspection_id IS NULL)"));
                result.clouds = count(QStringLiteral("SELECT count(*) FROM point_cloud c "
                    "JOIN dataset d ON d.dataset_id=c.dataset_id "
                    "WHERE (?='' OR d.inspection_id=? OR d.inspection_id IS NULL)"));
                result.images = count(QStringLiteral("SELECT count(*) FROM image i "
                    "JOIN dataset d ON d.dataset_id=i.dataset_id "
                    "WHERE (?='' OR d.inspection_id=? OR d.inspection_id IS NULL)"));
                result.corrected = count(QStringLiteral("SELECT count(*) FROM image_variant v "
                    "JOIN image i ON i.image_id=v.image_id JOIN dataset d ON d.dataset_id=i.dataset_id "
                    "WHERE v.variant_type='DEVICE_UNDISTORTED' AND v.status='VALID' AND v.is_active=1 "
                    "AND (?='' OR d.inspection_id=? OR d.inspection_id IS NULL)"));
                result.alignedPoses = count(QStringLiteral("SELECT count(*) FROM camera_pose p "
                    "JOIN image i ON i.image_id=p.image_id JOIN dataset d ON d.dataset_id=i.dataset_id "
                    "WHERE p.pose_stage='MODEL_ALIGNED' "
                    "AND (?='' OR d.inspection_id=? OR d.inspection_id IS NULL)"));
            }
            if (tables.contains(QStringLiteral("registration_result"))) {
                if (db.record(QStringLiteral("registration_result")).indexOf(QStringLiteral("inspection_id")) >= 0)
                    result.registrations = count(QStringLiteral("SELECT count(*) FROM registration_result r "
                        "WHERE r.active=1 AND (?='' OR r.inspection_id=? OR r.inspection_id IS NULL)"));
                else {
                    QSqlQuery query(db);
                    if (query.exec(QStringLiteral("SELECT count(*) FROM registration_result WHERE active=1")) && query.next())
                        result.registrations = query.value(0).toInt();
                }
            }
            if (tables.contains(QStringLiteral("damage_result"))) {
                result.damageImages = count(QStringLiteral("SELECT count(*) FROM damage_result r "
                    "JOIN dataset d ON d.dataset_id=r.dataset_id WHERE r.active=1 "
                    "AND (?='' OR d.inspection_id=? OR d.inspection_id IS NULL)"));
                result.damageObjects = count(QStringLiteral("SELECT count(*) FROM damage_object o "
                    "JOIN damage_result r ON r.result_id=o.result_id JOIN dataset d ON d.dataset_id=r.dataset_id "
                    "WHERE r.active=1 AND o.review_state!='REJECTED' AND substr(o.type,1,7)!='target_' "
                    "AND (?='' OR d.inspection_id=? OR d.inspection_id IS NULL)"));
                const bool tracksReview = db.record(QStringLiteral("damage_object"))
                    .indexOf(QStringLiteral("review_confirmed")) >= 0;
                result.damagePending = count(QStringLiteral("SELECT count(*) FROM damage_object o "
                    "JOIN damage_result r ON r.result_id=o.result_id JOIN dataset d ON d.dataset_id=r.dataset_id "
                    "WHERE r.active=1 AND ") +
                    (tracksReview ? QStringLiteral("(o.review_confirmed=0 OR o.review_state='HOLD') ") :
                        QStringLiteral("o.review_state IN ('UNREVIEWED','HOLD') ")) +
                    QStringLiteral("AND substr(o.type,1,7)!='target_' "
                    "AND (?='' OR d.inspection_id=? OR d.inspection_id IS NULL)"));
            }
            if (tables.contains(QStringLiteral("map_projection_run")) &&
                tables.contains(QStringLiteral("map_projection_item"))) {
                result.mapRuns = count(QStringLiteral("SELECT count(*) FROM map_projection_run r "
                    "WHERE r.active=1 AND (?='' OR r.inspection_id=?)"));
                result.projectedDamages = count(QStringLiteral("SELECT count(*) FROM map_projection_item i "
                    "JOIN map_projection_run r ON r.run_id=i.run_id WHERE r.active=1 "
                    "AND i.status='PROJECTED' AND (?='' OR r.inspection_id=?)"));
                result.projectionPending = count(QStringLiteral("SELECT count(*) FROM map_projection_item i "
                    "JOIN map_projection_run r ON r.run_id=i.run_id WHERE r.active=1 "
                    "AND i.status!='PROJECTED' AND (?='' OR r.inspection_id=?)"));
                result.mapParts = count(QStringLiteral("SELECT count(DISTINCT i.part_id) "
                    "FROM map_projection_item i JOIN map_projection_run r ON r.run_id=i.run_id "
                    "WHERE r.active=1 AND i.status='PROJECTED' AND i.part_id!='' "
                    "AND (?='' OR r.inspection_id=?)"));
                result.measuredDamages = count(QStringLiteral("SELECT count(*) FROM map_projection_item i "
                    "JOIN map_projection_run r ON r.run_id=i.run_id WHERE r.active=1 "
                    "AND i.status='PROJECTED' AND (i.length_mm>0 OR i.area_mm2>0) "
                    "AND (?='' OR r.inspection_id=?)"));
            }
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}

QStringList recentPaths() {
    return QSettings(QStringLiteral("InspectMap"), QStringLiteral("ProjectManager"))
        .value(QStringLiteral("recentProjects")).toStringList();
}

void saveRecent(const QStringList &paths) {
    QSettings(QStringLiteral("InspectMap"), QStringLiteral("ProjectManager"))
        .setValue(QStringLiteral("recentProjects"), paths);
}

QLabel *heading(const QString &text) {
    auto *label = new QLabel(text);
    label->setObjectName(QStringLiteral("section"));
    return label;
}

class ProjectWindow : public QMainWindow {
public:
    ProjectWindow() {
        setWindowTitle(QStringLiteral("InspectMap | 프로젝트 관리자"));
        resize(1120, 700);
        setMinimumSize(760, 520);
        setStyleSheet(QStringLiteral(R"(
            QWidget { background:#f6f8f8; color:#203039; font-size:13px; }
            QWidget#header { background:#263b4e; }
            QWidget#header QLabel { background:transparent; color:white; font-size:19px; font-weight:700; }
            QLabel#section { font-size:16px; font-weight:600; }
            QLabel#muted { color:#66777e; }
            QPushButton { background:white; border:1px solid #c8d4d6; border-radius:4px; padding:8px 14px; }
            QPushButton:hover { border-color:#14796f; }
            QPushButton#primary { background:#14796f; color:white; border-color:#14796f; }
            QLineEdit,QTableWidget,QTreeWidget { background:white; border:1px solid #c8d4d6; }
            QLineEdit { padding:7px; }
            QTableWidget::item { padding:6px; }
            QTableWidget::item:selected,QTreeWidget::item:selected { background:#dcefeb; color:#203039; }
            QHeaderView::section { background:#e9eeee; padding:8px; border:0; }
            QTabWidget::pane { border:1px solid #c8d4d6; background:white; }
            QTabBar::tab {
                background:#e8eeef; color:#4d626a; border:1px solid #c8d4d6;
                border-bottom:3px solid #c8d4d6; padding:10px 18px;
            }
            QTabBar::tab:selected {
                background:white; color:#0d6159; font-weight:700;
                border-bottom:3px solid #14796f;
            }
            QTabBar::tab:disabled { background:#f1f3f3; color:#9ba7aa; }
            QTabBar::tab:hover:!selected:!disabled { background:#dcefeb; color:#203039; }
        )"));
        buildUi();
        refreshRecent();
    }

private:
    void buildUi() {
        auto *root = new QWidget;
        auto *layout = new QVBoxLayout(root);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        auto *header = new QWidget;
        header->setObjectName(QStringLiteral("header"));
        auto *headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(22, 14, 22, 14);
        headerLayout->addWidget(new QLabel(QStringLiteral("InspectMap · 프로젝트 관리자")));
        headerLayout->addStretch();
        layout->addWidget(header);

        auto *toolbar = new QWidget;
        auto *tools = new QHBoxLayout(toolbar);
        tools->setContentsMargins(18, 10, 18, 10);
        auto *create = new QPushButton(QStringLiteral("새 프로젝트"));
        create->setObjectName(QStringLiteral("primary"));
        auto *open = new QPushButton(QStringLiteral("프로젝트 열기"));
        tools->addWidget(create);
        tools->addWidget(open);
        tools->addStretch();
        scope_ = new QLabel(QStringLiteral("프로젝트 미선택"));
        scope_->setObjectName(QStringLiteral("muted"));
        tools->addWidget(scope_);
        layout->addWidget(toolbar);
        connect(create, &QPushButton::clicked, this, [this] { createProject(); });
        connect(open, &QPushButton::clicked, this, [this] { browseProject(); });

        auto *splitter = new QSplitter(Qt::Horizontal);
        auto *navigation = new QWidget;
        auto *navLayout = new QVBoxLayout(navigation);
        navLayout->addWidget(heading(QStringLiteral("시설물 · 점검")));
        tree_ = new QTreeWidget;
        tree_->setHeaderHidden(true);
        navLayout->addWidget(tree_, 1);
        datasetsLabel_ = new QLabel(QStringLiteral("등록된 데이터 세트 없음"));
        datasetsLabel_->setObjectName(QStringLiteral("muted"));
        datasetsLabel_->setWordWrap(true);
        navLayout->addWidget(datasetsLabel_);
        navLayout->addWidget(heading(QStringLiteral("프로그램")));
        const struct { const char *executable; const char *label; } programs[] = {
            {"DataConverter", "데이터 변환기"},
            {"RegistrationTool", "정합 작업기"},
            {"AIDamageManager", "AI 손상정보 관리자"},
            {"InspectionMapEditor", "외관망도 작성기"}
        };
        for (const auto &program : programs) {
            auto *button = new QPushButton(QString::fromUtf8(program.label));
            const auto executable = QString::fromLatin1(program.executable);
            button->setEnabled(false);
            button->setToolTip(QStringLiteral("선택한 프로젝트를 이 프로그램에서 엽니다."));
            navLayout->addWidget(button);
            programButtons_.append(button);
            connect(button, &QPushButton::clicked, this, [this, executable] { launchProgram(executable); });
        }
        splitter->addWidget(navigation);

        tabs_ = new QTabWidget;
        tabs_->addTab(buildProjectTab(), QStringLiteral("프로젝트 선택"));
        tabs_->addTab(buildWorkTab(), QStringLiteral("작업 현황"));
        tabs_->addTab(buildFacilityTab(), QStringLiteral("시설물·점검"));
        tabs_->addTab(buildPipelineTab(), QStringLiteral("파이프라인"));
        tabs_->addTab(buildHistoryTab(), QStringLiteral("산출물·이력"));
        for (int i = 1; i < tabs_->count(); ++i) tabs_->setTabEnabled(i, false);
        connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
            if (index == 1 && !projectDb_.isEmpty()) { refreshWork(); refreshHistory(); }
        });
        splitter->addWidget(tabs_);

        auto *details = new QWidget;
        auto *detailLayout = new QVBoxLayout(details);
        detailLayout->addWidget(heading(QStringLiteral("현재 선택")));
        projectName_ = new QLabel(QStringLiteral("프로젝트 없음"));
        projectName_->setWordWrap(true);
        detailLayout->addWidget(projectName_);
        projectId_ = new QLabel;
        projectId_->setObjectName(QStringLiteral("muted"));
        projectId_->setWordWrap(true);
        detailLayout->addWidget(projectId_);
        projectPath_ = new QLabel;
        projectPath_->setObjectName(QStringLiteral("muted"));
        projectPath_->setWordWrap(true);
        projectPath_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        detailLayout->addWidget(projectPath_);
        detailLayout->addWidget(heading(QStringLiteral("등록 입력")));
        inputDetail_ = new QLabel;
        inputDetail_->setObjectName(QStringLiteral("muted"));
        inputDetail_->setWordWrap(true);
        inputDetail_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        detailLayout->addWidget(inputDetail_);
        detailLayout->addWidget(heading(QStringLiteral("선택 작업")));
        selectionDetail_ = new QLabel(QStringLiteral("작업을 선택하세요."));
        selectionDetail_->setWordWrap(true);
        detailLayout->addWidget(selectionDetail_);
        detailLayout->addStretch();
        splitter->addWidget(details);
        splitter->setSizes({230, 650, 240});
        layout->addWidget(splitter, 1);
        status_ = new QLabel(QStringLiteral("프로젝트를 열거나 생성하세요."));
        status_->setObjectName(QStringLiteral("muted"));
        status_->setContentsMargins(18, 7, 18, 7);
        layout->addWidget(status_);
        setCentralWidget(root);
        connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive && !projectDb_.isEmpty()) {
                refreshWork();
                refreshHistory();
            }
        });
        connect(tree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item) {
            const auto facilityId = item->data(0, Qt::UserRole).toString();
            const auto inspectionId = item->data(0, Qt::UserRole + 1).toString();
            if (facilityId.isEmpty()) return;
            for (int row = 0; row < facilities_.size(); ++row) {
                if (facilities_[row].id != facilityId) continue;
                tabs_->setCurrentIndex(2);
                selectFacility(row);
                for (int i = 0; i < inspections_.size(); ++i)
                    if (inspections_[i].id == inspectionId) selectInspection(i);
                break;
            }
        });
    }

    QWidget *buildProjectTab() {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(18, 16, 18, 16);
        layout->addWidget(heading(QStringLiteral("최근 프로젝트")));
        recent_ = new QTableWidget(0, 3);
        recent_->setHorizontalHeaderLabels({QStringLiteral("프로젝트"), QStringLiteral("위치"), QStringLiteral("상태")});
        recent_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        recent_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        recent_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        recent_->setSelectionBehavior(QAbstractItemView::SelectRows);
        recent_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        recent_->verticalHeader()->hide();
        layout->addWidget(recent_, 1);
        connect(recent_, &QTableWidget::itemDoubleClicked, this, [this] { openSelected(); });
        auto *actions = new QHBoxLayout;
        auto *forget = new QPushButton(QStringLiteral("목록에서 제거"));
        auto *reconnect = new QPushButton(QStringLiteral("경로 다시 연결"));
        auto *openSelectedButton = new QPushButton(QStringLiteral("선택 프로젝트 열기"));
        actions->addWidget(forget);
        actions->addWidget(reconnect);
        actions->addStretch();
        actions->addWidget(openSelectedButton);
        layout->addLayout(actions);
        connect(openSelectedButton, &QPushButton::clicked, this, [this] { openSelected(); });
        connect(forget, &QPushButton::clicked, this, [this] {
            if (recent_->currentRow() < 0) return;
            auto paths = recentPaths();
            paths.removeAll(recent_->item(recent_->currentRow(), 1)->text());
            saveRecent(paths);
            refreshRecent();
        });
        connect(reconnect, &QPushButton::clicked, this, [this] {
            if (recent_->currentRow() < 0) return;
            const auto oldPath = recent_->item(recent_->currentRow(), 1)->text();
            const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("project.sqlite 선택"), {}, QStringLiteral("project.sqlite (project.sqlite)"));
            if (path.isEmpty()) return;
            ProjectInfo info;
            QString error;
            if (!ProjectStore::read(path, &info, &error)) {
                QMessageBox::warning(this, QStringLiteral("연결 실패"), error);
                return;
            }
            auto paths = recentPaths();
            paths.removeAll(oldPath);
            saveRecent(paths);
            openProject(path);
        });
        return page;
    }

    QWidget *buildWorkTab() {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(18, 16, 18, 16);
        layout->addWidget(heading(QStringLiteral("모듈별 작업 현황")));
        workTable_ = new QTableWidget(std::size(modules), 5);
        workTable_->setHorizontalHeaderLabels({QStringLiteral("모듈"), QStringLiteral("범위"),
            QStringLiteral("준비"), QStringLiteral("현재 결과"), QStringLiteral("검토")});
        configureTable(workTable_);
        workTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        layout->addWidget(workTable_, 1);
        connect(workTable_, &QTableWidget::cellClicked, this, [this](int row) {
            if (row < 0 || row >= int(std::size(modules))) return;
            selectionDetail_->setText(QString::fromUtf8(modules[row].name) + QStringLiteral("\n담당 프로그램: ") +
                QString::fromLatin1(modules[row].program) + QStringLiteral("\n준비: ") +
                workTable_->item(row, 2)->text() + QStringLiteral("\n현재 결과: ") +
                workTable_->item(row, 3)->text() + QStringLiteral("\n검토: ") +
                workTable_->item(row, 4)->text());
            const bool registration = row == 4 && !pendingReviews_.isEmpty();
            openCandidate_->setEnabled(registration);
            confirmCandidate_->setEnabled(registration);
        });
        auto *actions = new QHBoxLayout;
        auto *openProgram = new QPushButton(QStringLiteral("선택 작업의 프로그램 열기"));
        openCandidate_ = new QPushButton(QStringLiteral("정합 후보 열기"));
        confirmCandidate_ = new QPushButton(QStringLiteral("검토 확정"));
        openCandidate_->setEnabled(false);
        confirmCandidate_->setEnabled(false);
        auto *refresh = new QPushButton(QStringLiteral("현황 새로고침"));
        actions->addWidget(openProgram);
        actions->addWidget(openCandidate_);
        actions->addWidget(confirmCandidate_);
        actions->addWidget(refresh);
        actions->addStretch();
        layout->addLayout(actions);
        connect(openProgram, &QPushButton::clicked, this, [this] {
            const int row = workTable_->currentRow();
            if (row >= 0 && row < int(std::size(modules)) && row != 0)
                launchProgram(QString::fromLatin1(modules[row].program));
        });
        connect(openCandidate_, &QPushButton::clicked, this, [this] {
            const QString reviewId = choosePendingRegistration();
            if (!reviewId.isEmpty()) launchProgram(QStringLiteral("RegistrationTool"),
                {QStringLiteral("--review"), reviewId});
        });
        connect(confirmCandidate_, &QPushButton::clicked, this, [this] {
            const QString reviewId = choosePendingRegistration();
            if (reviewId.isEmpty()) return;
            const auto item = std::find_if(pendingReviews_.cbegin(), pendingReviews_.cend(),
                [&](const RegistrationReviewInfo &candidate) { return candidate.id == reviewId; });
            if (item == pendingReviews_.cend()) return;
            if (QMessageBox::question(this, QStringLiteral("정합 결과 확정"),
                QStringLiteral("%1\n%2\n표면 RMSE %3 m · 0.5 m 이내 %4%\n\n"
                               "이 후보를 검토하고 확정하시겠습니까? 기존 활성 정합이 교체됩니다.")
                    .arg(QFileInfo(item->cloudPath).fileName(), item->method)
                    .arg(item->surfaceRmse, 0, 'f', 3).arg(item->overlap * 100, 0, 'f', 1)) != QMessageBox::Yes)
                return;
            QString error;
            int poses = 0;
            if (!ProjectStore::confirmRegistration(projectDb_, reviewId, &poses, &error)) {
                QMessageBox::warning(this, QStringLiteral("정합 확정 실패"), error);
                return;
            }
            refreshWork();
            refreshHistory();
            status_->setText(QStringLiteral("정합 결과 확정 · 카메라 pose %1장 갱신").arg(poses));
        });
        connect(refresh, &QPushButton::clicked, this, [this] { refreshWork(); refreshHistory(); });
        return page;
    }

    QWidget *buildPipelineTab() {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(18, 16, 18, 16);
        layout->addWidget(heading(QStringLiteral("전체 처리 파이프라인")));
        auto *scopeNote = new QLabel(QStringLiteral("선택 점검을 대상으로 각 단계의 실행 방식을 설정합니다."));
        scopeNote->setObjectName(QStringLiteral("muted"));
        layout->addWidget(scopeNote);
        pipelineTable_ = new QTableWidget(std::size(modules) - 1, 3);
        pipelineTable_->setHorizontalHeaderLabels({QStringLiteral("처리 단계"), QStringLiteral("담당 프로그램"), QStringLiteral("실행 방식")});
        configureTable(pipelineTable_);
        pipelineTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        layout->addWidget(pipelineTable_, 1);
        for (int row = 0; row < pipelineTable_->rowCount(); ++row) {
            const auto &module = modules[row + 1];
            pipelineTable_->setItem(row, 0, new QTableWidgetItem(QString::fromUtf8(module.name)));
            pipelineTable_->setItem(row, 1, new QTableWidgetItem(QString::fromLatin1(module.program)));
            auto *mode = new QComboBox;
            mode->addItems({QStringLiteral("미설정"), QStringLiteral("AUTO"), QStringLiteral("REVIEW"),
                QStringLiteral("MANUAL"), QStringLiteral("SKIP")});
            pipelineTable_->setCellWidget(row, 2, mode);
            connect(mode, &QComboBox::currentTextChanged, this, [this, row](const QString &value) {
                if (loadingModes_ || projectDb_.isEmpty() || value == QStringLiteral("미설정")) return;
                QString error;
                if (!ProjectStore::setPipelineMode(projectDb_, QString::fromLatin1(modules[row + 1].id), value, &error))
                    QMessageBox::warning(this, QStringLiteral("실행 방식 저장 실패"), error);
            });
        }
        auto *runRow = new QHBoxLayout;
        runAll_ = new QPushButton(QStringLiteral("전체 실행"));
        runAll_->setObjectName(QStringLiteral("primary"));
        runAll_->setEnabled(false);
        runAll_->setToolTip(QStringLiteral("InspectionWorker와 처리 엔진 구현 후 사용할 수 있습니다."));
        runRow->addWidget(runAll_);
        runRow->addStretch();
        layout->addLayout(runRow);
        pipelineNote_ = new QLabel(QStringLiteral("InspectionWorker와 처리 엔진이 아직 없어 전체 실행은 사용할 수 없습니다."));
        pipelineNote_->setObjectName(QStringLiteral("muted"));
        pipelineNote_->setWordWrap(true);
        layout->addWidget(pipelineNote_);
        return page;
    }

    QWidget *buildHistoryTab() {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(18, 16, 18, 16);
        layout->addWidget(heading(QStringLiteral("실행 이력")));
        runTable_ = new QTableWidget(0, 4);
        runTable_->setHorizontalHeaderLabels({QStringLiteral("모듈"), QStringLiteral("상태"),
            QStringLiteral("시작"), QStringLiteral("종료")});
        configureTable(runTable_);
        layout->addWidget(runTable_, 1);
        layout->addWidget(heading(QStringLiteral("산출물")));
        artifactTable_ = new QTableWidget(0, 4);
        artifactTable_->setHorizontalHeaderLabels({QStringLiteral("유형"), QStringLiteral("상태"),
            QStringLiteral("채택"), QStringLiteral("위치")});
        configureTable(artifactTable_);
        layout->addWidget(artifactTable_, 1);
        auto *refresh = new QPushButton(QStringLiteral("이력 새로고침"));
        layout->addWidget(refresh, 0, Qt::AlignLeft);
        connect(refresh, &QPushButton::clicked, this, [this] { refreshHistory(); });
        connect(artifactTable_, &QTableWidget::cellClicked, this, [this](int row) {
            if (row >= 0 && row < artifactTable_->rowCount())
                selectionDetail_->setText(artifactTable_->item(row, 3)->text());
        });
        return page;
    }

    QWidget *buildFacilityTab() {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(18, 16, 18, 16);
        layout->addWidget(heading(QStringLiteral("시설물")));
        facilityTable_ = new QTableWidget(0, 2);
        facilityTable_->setHorizontalHeaderLabels({QStringLiteral("시설물명"), QStringLiteral("종류")});
        configureTable(facilityTable_);
        layout->addWidget(facilityTable_, 1);
        connect(facilityTable_, &QTableWidget::cellClicked, this, [this](int row) { selectFacility(row); });
        auto *facilityActions = new QHBoxLayout;
        auto *addFacility = new QPushButton(QStringLiteral("시설물 추가"));
        auto *editFacilityButton = new QPushButton(QStringLiteral("선택 시설물 편집"));
        facilityActions->addWidget(addFacility);
        facilityActions->addWidget(editFacilityButton);
        facilityActions->addStretch();
        layout->addLayout(facilityActions);
        connect(addFacility, &QPushButton::clicked, this, [this] { editFacility(true); });
        connect(editFacilityButton, &QPushButton::clicked, this, [this] { editFacility(false); });

        layout->addWidget(heading(QStringLiteral("선택 시설물의 점검")));
        inspectionTable_ = new QTableWidget(0, 3);
        inspectionTable_->setHorizontalHeaderLabels({QStringLiteral("점검명"), QStringLiteral("종류"), QStringLiteral("기간")});
        configureTable(inspectionTable_);
        layout->addWidget(inspectionTable_, 1);
        connect(inspectionTable_, &QTableWidget::cellClicked, this, [this](int row) { selectInspection(row); });
        auto *inspectionActions = new QHBoxLayout;
        auto *addInspection = new QPushButton(QStringLiteral("점검 추가"));
        auto *editInspectionButton = new QPushButton(QStringLiteral("선택 점검 편집"));
        inspectionActions->addWidget(addInspection);
        inspectionActions->addWidget(editInspectionButton);
        inspectionActions->addStretch();
        layout->addLayout(inspectionActions);
        connect(addInspection, &QPushButton::clicked, this, [this] { editInspection(true); });
        connect(editInspectionButton, &QPushButton::clicked, this, [this] { editInspection(false); });
        return page;
    }

    void configureTable(QTableWidget *table) {
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->verticalHeader()->hide();
    }

    void refreshRecent() {
        const auto paths = recentPaths();
        recent_->setRowCount(paths.size());
        for (int row = 0; row < paths.size(); ++row) {
            ProjectInfo info;
            QString error;
            const bool valid = ProjectStore::read(paths[row], &info, &error);
            recent_->setItem(row, 0, new QTableWidgetItem(valid ? info.name : QFileInfo(paths[row]).dir().dirName()));
            recent_->setItem(row, 1, new QTableWidgetItem(paths[row]));
            recent_->setItem(row, 2, new QTableWidgetItem(valid ? QStringLiteral("사용 가능") : QStringLiteral("연결 필요")));
        }
    }

    void openSelected() {
        if (recent_->currentRow() >= 0) openProject(recent_->item(recent_->currentRow(), 1)->text());
    }

    void browseProject() {
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("project.sqlite 열기"), {}, QStringLiteral("project.sqlite (project.sqlite)"));
        if (!path.isEmpty()) openProject(path);
    }

    void createProject() {
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("새 프로젝트"));
        dialog.setMinimumWidth(500);
        auto *layout = new QVBoxLayout(&dialog);
        auto *form = new QFormLayout;
        auto *name = new QLineEdit;
        auto *path = new QLineEdit;
        auto *browse = new QPushButton(QStringLiteral("찾아보기"));
        auto *pathRow = new QWidget;
        auto *pathLayout = new QHBoxLayout(pathRow);
        pathLayout->setContentsMargins(0, 0, 0, 0);
        pathLayout->addWidget(path);
        pathLayout->addWidget(browse);
        form->addRow(QStringLiteral("프로젝트명"), name);
        form->addRow(QStringLiteral("빈 프로젝트 폴더"), pathRow);
        auto *facilityName = new QLineEdit;
        auto *inspectionName = new QLineEdit;
        auto *model = new QLineEdit;
        auto *cloud = new QLineEdit;
        auto *browseModel = new QPushButton(QStringLiteral("찾아보기"));
        auto *browseCloud = new QPushButton(QStringLiteral("찾아보기"));
        auto *modelRow = new QWidget;
        auto *modelLayout = new QHBoxLayout(modelRow);
        modelLayout->setContentsMargins(0, 0, 0, 0);
        modelLayout->addWidget(model);
        modelLayout->addWidget(browseModel);
        auto *cloudRow = new QWidget;
        auto *cloudLayout = new QHBoxLayout(cloudRow);
        cloudLayout->setContentsMargins(0, 0, 0, 0);
        cloudLayout->addWidget(cloud);
        cloudLayout->addWidget(browseCloud);
        form->addRow(new QLabel(QStringLiteral("빠른 시작 (선택)")));
        form->addRow(QStringLiteral("시설물명"), facilityName);
        form->addRow(QStringLiteral("점검명"), inspectionName);
        form->addRow(QStringLiteral("3D 모델 폴더"), modelRow);
        form->addRow(QStringLiteral("LiDAR LAS"), cloudRow);
        layout->addLayout(form);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("생성"));
        layout->addWidget(buttons);
        connect(browse, &QPushButton::clicked, &dialog, [&] {
            const auto selected = QFileDialog::getExistingDirectory(&dialog, QStringLiteral("빈 프로젝트 폴더 선택"));
            if (!selected.isEmpty()) path->setText(selected);
        });
        connect(browseModel, &QPushButton::clicked, &dialog, [&] {
            const auto selected = QFileDialog::getExistingDirectory(&dialog, QStringLiteral("3D 모델 폴더 선택"));
            if (!selected.isEmpty()) model->setText(selected);
        });
        connect(browseCloud, &QPushButton::clicked, &dialog, [&] {
            const auto selected = QFileDialog::getOpenFileName(&dialog, QStringLiteral("LiDAR LAS 선택"), {}, QStringLiteral("LAS (*.las)"));
            if (!selected.isEmpty()) cloud->setText(selected);
        });
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            QString error;
            const ProjectSetup setup{facilityName->text().trimmed(), inspectionName->text().trimmed(),
                                     model->text().trimmed(), cloud->text().trimmed()};
            if (!ProjectStore::create(path->text().trimmed(), name->text(), setup, &error)) {
                QMessageBox::warning(&dialog, QStringLiteral("프로젝트 생성 실패"), error);
                return;
            }
            dialog.accept();
            openProject(ProjectStore::databasePath(path->text().trimmed()));
        });
        dialog.exec();
    }

    void openProject(const QString &path) {
        ProjectInfo info;
        QString error;
        if (!ProjectStore::ensureManagerSchema(path, &error) || !ProjectStore::read(path, &info, &error)) {
            QMessageBox::warning(this, QStringLiteral("프로젝트 열기 실패"), error);
            refreshRecent();
            return;
        }
        projectDb_ = QFileInfo(path).absoluteFilePath();
        auto paths = recentPaths();
        paths.removeAll(projectDb_);
        paths.prepend(projectDb_);
        while (paths.size() > 12) paths.removeLast();
        saveRecent(paths);
        refreshRecent();
        projectName_->setText(info.name);
        projectId_->setText(info.id);
        projectPath_->setText(projectDb_);
        for (int i = 1; i < tabs_->count(); ++i) tabs_->setTabEnabled(i, true);
        for (auto *button : programButtons_) button->setEnabled(true);
        selectedFacilityId_.clear();
        selectedInspectionId_.clear();
        refreshFacilities();
        refreshPipeline();
        refreshWork();
        refreshHistory();
        status_->setText(QStringLiteral("프로젝트 연결됨 · 전체 실행은 Worker 구현 전까지 비활성"));
        tabs_->setCurrentIndex(1);
    }

    QString choosePendingRegistration() {
        if (pendingReviews_.isEmpty()) return {};
        QStringList labels;
        for (const auto &item : pendingReviews_)
            labels.append(QStringLiteral("%1 · %2 · %3 · %4")
                .arg(QFileInfo(item.cloudPath).fileName(), item.method, item.createdAt, item.id.left(8)));
        bool accepted = false;
        const QString choice = QInputDialog::getItem(this, QStringLiteral("미검토 정합 후보"),
            QStringLiteral("검토할 결과"), labels, 0, false, &accepted);
        const int index = labels.indexOf(choice);
        return accepted && index >= 0 ? pendingReviews_[index].id : QString();
    }

    void launchProgram(const QString &name, const QStringList &extra = {}) {
        if (projectDb_.isEmpty()) return;
        const QString executable = QDir(QCoreApplication::applicationDirPath()).filePath(name + QStringLiteral(".exe"));
        if (!QFileInfo(executable).isFile()) {
            QMessageBox::information(this, QStringLiteral("프로그램 준비 중"),
                QStringLiteral("%1 실행 파일이 아직 설치되지 않았습니다.\n%2").arg(name, executable));
            return;
        }
        QStringList arguments{QStringLiteral("--project"), projectDb_};
        if (!selectedFacilityId_.isEmpty()) arguments << QStringLiteral("--facility") << selectedFacilityId_;
        if (!selectedInspectionId_.isEmpty()) arguments << QStringLiteral("--inspection") << selectedInspectionId_;
        arguments << extra;
        if (!QProcess::startDetached(executable, arguments, QFileInfo(executable).absolutePath()))
            QMessageBox::warning(this, QStringLiteral("프로그램 실행 실패"), executable);
    }

    void refreshWork() {
        if (projectDb_.isEmpty()) return;
        QString error;
        if (!ProjectStore::pendingRegistrations(projectDb_, selectedInspectionId_, &pendingReviews_, &error)) {
            status_->setText(QStringLiteral("정합 후보를 읽지 못했습니다: ") + error);
            pendingReviews_.clear();
        }
        const bool registrationSelected = workTable_->currentRow() == 4 && !pendingReviews_.isEmpty();
        openCandidate_->setEnabled(registrationSelected);
        confirmCandidate_->setEnabled(registrationSelected);
        const WorkCounts counts = readWorkCounts(projectDb_, selectedInspectionId_);
        datasetsLabel_->setText(counts.datasets > 0 ?
            QStringLiteral("데이터 세트 %1개 · 이미지 %2장 · 보정 %3장")
                .arg(counts.datasets).arg(counts.images).arg(counts.corrected) :
            QStringLiteral("등록된 데이터 세트 없음"));
        QString modelPath, cloudPath;
        if (!selectedFacilityId_.isEmpty())
            ProjectStore::facilityModelPath(projectDb_, selectedFacilityId_, &modelPath, &error);
        if (!selectedInspectionId_.isEmpty())
            ProjectStore::inspectionCloudPath(projectDb_, selectedInspectionId_, &cloudPath, &error);
        const bool hasModel = QFileInfo(modelPath).isDir();
        const bool hasCloud = QFileInfo(cloudPath).isFile() || counts.clouds > 0;
        for (int row = 0; row < int(std::size(modules)); ++row) {
            const auto &module = modules[row];
            QString review = QStringLiteral("—");
            const QString scope = row == 0 ? QStringLiteral("프로젝트") :
                (selectedInspectionId_.isEmpty() ? QStringLiteral("점검 미선택") : QStringLiteral("선택 점검"));
            QString readiness = row == 0 ? QStringLiteral("준비") : QStringLiteral("미구현");
            QString result = QStringLiteral("—");
            if (row == 1) {
                readiness = QStringLiteral("실행 가능");
                if (counts.datasets > 0)
                    result = QStringLiteral("데이터 세트 %1개 · 이미지 %2장 · 보정 %3장")
                        .arg(counts.datasets).arg(counts.images).arg(counts.corrected);
            } else if (row == 2) {
                readiness = hasModel ? QStringLiteral("모델 입력 있음") : QStringLiteral("모델 필요");
                if (hasModel) result = QStringLiteral("모델 경로 등록");
            } else if (row == 4) {
                readiness = hasModel && hasCloud ? QStringLiteral("실행 가능") : QStringLiteral("모델·LAS 필요");
                if (counts.registrations > 0)
                    result = QStringLiteral("확정 정합 %1건").arg(counts.registrations);
                review = pendingReviews_.isEmpty() ?
                    counts.registrations > 0 ? QStringLiteral("확정") : QStringLiteral("—") :
                    QStringLiteral("미검토 %1건").arg(pendingReviews_.size());
            } else if (row == 5) {
                readiness = counts.registrations > 0 ? QStringLiteral("정합 결과 있음") : QStringLiteral("정합 필요");
                if (counts.alignedPoses > 0)
                    result = QStringLiteral("모델 좌표 pose %1장").arg(counts.alignedPoses);
                if (counts.alignedPoses > 0) review = QStringLiteral("확정");
            } else if (row == 7) {
                readiness = counts.images > 0 ? QStringLiteral("실행 가능") : QStringLiteral("영상 필요");
                if (counts.damageImages > 0)
                    result = QStringLiteral("결과 %1장 · 손상 %2개")
                        .arg(counts.damageImages).arg(counts.damageObjects);
                if (counts.damagePending > 0) review = QStringLiteral("검토 필요 %1개").arg(counts.damagePending);
                else if (counts.damageImages > 0) review = QStringLiteral("검토 완료");
            } else if (row == 8) {
                readiness = counts.alignedPoses > 0 && counts.damageObjects > 0 ?
                    QStringLiteral("실행 가능") : QStringLiteral("pose·채택 손상 필요");
                if (counts.mapRuns > 0)
                    result = QStringLiteral("투영 %1건 · 확인 필요 %2건")
                        .arg(counts.projectedDamages).arg(counts.projectionPending);
                if (counts.projectionPending > 0)
                    review = QStringLiteral("확인 필요 %1건").arg(counts.projectionPending);
            } else if (row == 9) {
                readiness = counts.mapRuns > 0 ? QStringLiteral("계산 결과 있음") : QStringLiteral("투영 필요");
                if (counts.mapRuns > 0)
                    result = QStringLiteral("3D 물량 %1건 · 초안").arg(counts.measuredDamages);
            } else if (row == 10) {
                readiness = counts.mapRuns > 0 ? QStringLiteral("도면 초안 가능") : QStringLiteral("투영 필요");
                if (counts.mapRuns > 0)
                    result = QStringLiteral("부재 %1개 · 망도/HTML/PDF/Excel 출력 가능").arg(counts.mapParts);
                if (counts.projectionPending > 0)
                    review = QStringLiteral("확인 필요 %1건").arg(counts.projectionPending);
            }
            const QStringList cells{QString::fromUtf8(module.name), scope, readiness, result, review};
            for (int col = 0; col < cells.size(); ++col)
                workTable_->setItem(row, col, new QTableWidgetItem(cells[col]));
        }
    }

    void refreshPipeline() {
        if (projectDb_.isEmpty()) return;
        QMap<QString, QString> modes;
        QString error;
        if (!ProjectStore::pipelineModes(projectDb_, &modes, &error)) {
            pipelineNote_->setText(QStringLiteral("실행 방식 조회 불가: ") + error);
            return;
        }
        loadingModes_ = true;
        for (int row = 0; row < pipelineTable_->rowCount(); ++row) {
            auto *mode = qobject_cast<QComboBox *>(pipelineTable_->cellWidget(row, 2));
            const QString saved = modes.value(QString::fromLatin1(modules[row + 1].id), QStringLiteral("미설정"));
            mode->setCurrentText(saved);
        }
        loadingModes_ = false;
    }

    void refreshHistory() {
        if (projectDb_.isEmpty()) return;
        QList<ModuleRunInfo> allRuns;
        QList<ArtifactInfo> artifacts;
        QString error;
        if (!ProjectStore::runs(projectDb_, {}, &allRuns, &error) ||
            !ProjectStore::artifacts(projectDb_, &artifacts, &error)) {
            status_->setText(QStringLiteral("산출물·이력 조회 실패: ") + error);
            return;
        }
        QList<ModuleRunInfo> runs;
        for (const auto &run : allRuns)
            if (selectedInspectionId_.isEmpty() || run.inspectionId == selectedInspectionId_ || run.inspectionId.isEmpty())
                runs.append(run);
        runTable_->setRowCount(runs.size());
        for (int row = 0; row < runs.size(); ++row) {
            const QStringList cells{runs[row].moduleId, runs[row].status, runs[row].startedAt, runs[row].finishedAt};
            for (int col = 0; col < cells.size(); ++col)
                runTable_->setItem(row, col, new QTableWidgetItem(cells[col]));
        }
        QSet<QString> visibleRunIds;
        for (const auto &run : runs) visibleRunIds.insert(run.id);
        QList<ArtifactInfo> visibleArtifacts;
        for (const auto &artifact : artifacts)
            if (selectedInspectionId_.isEmpty() || visibleRunIds.contains(artifact.runId)) visibleArtifacts.append(artifact);
        artifactTable_->setRowCount(visibleArtifacts.size());
        for (int row = 0; row < visibleArtifacts.size(); ++row) {
            const QStringList cells{visibleArtifacts[row].type, visibleArtifacts[row].status,
                visibleArtifacts[row].active ? QStringLiteral("예") : QStringLiteral("아니오"), visibleArtifacts[row].path};
            for (int col = 0; col < cells.size(); ++col)
                artifactTable_->setItem(row, col, new QTableWidgetItem(cells[col]));
        }
    }

    void refreshFacilities() {
        QString error;
        if (!ProjectStore::facilities(projectDb_, &facilities_, &error)) {
            QMessageBox::warning(this, QStringLiteral("시설물 조회 실패"), error);
            return;
        }
        facilityTable_->setRowCount(facilities_.size());
        for (int i = 0; i < facilities_.size(); ++i) {
            facilityTable_->setItem(i, 0, new QTableWidgetItem(facilities_[i].name));
            facilityTable_->setItem(i, 1, new QTableWidgetItem(facilities_[i].type));
        }
        int row = -1;
        for (int i = 0; i < facilities_.size(); ++i)
            if (facilities_[i].id == selectedFacilityId_) row = i;
        if (row < 0 && !facilities_.isEmpty()) row = 0;
        if (row >= 0) selectFacility(row);
        else {
            selectedFacilityId_.clear();
            selectedInspectionId_.clear();
            inspections_.clear();
            inspectionTable_->setRowCount(0);
            updateScope();
            refreshTree();
        }
    }

    void selectFacility(int row) {
        if (row < 0 || row >= facilities_.size()) return;
        facilityTable_->selectRow(row);
        selectedFacilityId_ = facilities_[row].id;
        selectedInspectionId_.clear();
        QString error;
        if (!ProjectStore::inspections(projectDb_, selectedFacilityId_, &inspections_, &error)) {
            QMessageBox::warning(this, QStringLiteral("점검 조회 실패"), error);
            return;
        }
        inspectionTable_->setRowCount(inspections_.size());
        for (int i = 0; i < inspections_.size(); ++i) {
            const auto &item = inspections_[i];
            inspectionTable_->setItem(i, 0, new QTableWidgetItem(item.name));
            inspectionTable_->setItem(i, 1, new QTableWidgetItem(item.type));
            inspectionTable_->setItem(i, 2, new QTableWidgetItem(item.startedAt + QStringLiteral(" ~ ") + item.endedAt));
        }
        if (inspections_.size() == 1) {
            selectedInspectionId_ = inspections_.first().id;
            inspectionTable_->selectRow(0);
        }
        updateScope();
        refreshTree();
        refreshWork();
        refreshHistory();
    }

    void selectInspection(int row) {
        if (row < 0 || row >= inspections_.size()) return;
        inspectionTable_->selectRow(row);
        selectedInspectionId_ = inspections_[row].id;
        updateScope();
        refreshWork();
        refreshHistory();
    }

    void updateScope() {
        QString text = projectName_->text();
        for (const auto &facility : facilities_)
            if (facility.id == selectedFacilityId_) text += QStringLiteral(" / ") + facility.name;
        for (const auto &inspection : inspections_)
            if (inspection.id == selectedInspectionId_) text += QStringLiteral(" / ") + inspection.name;
        scope_->setText(text);
        QString model, cloud, error;
        if (!selectedFacilityId_.isEmpty())
            ProjectStore::facilityModelPath(projectDb_, selectedFacilityId_, &model, &error);
        if (!selectedInspectionId_.isEmpty())
            ProjectStore::inspectionCloudPath(projectDb_, selectedInspectionId_, &cloud, &error);
        inputDetail_->setText(QStringLiteral("모델: %1\nLAS: %2")
            .arg(model.isEmpty() ? QStringLiteral("미등록") : model,
                 cloud.isEmpty() ? QStringLiteral("미등록") : cloud));
    }

    void refreshTree() {
        tree_->clear();
        auto *project = new QTreeWidgetItem(tree_, {projectName_->text()});
        for (const auto &facility : facilities_) {
            auto *node = new QTreeWidgetItem(project, {facility.name});
            node->setData(0, Qt::UserRole, facility.id);
            if (facility.id != selectedFacilityId_) continue;
            for (const auto &inspection : inspections_) {
                auto *child = new QTreeWidgetItem(node, {inspection.name});
                child->setData(0, Qt::UserRole, facility.id);
                child->setData(0, Qt::UserRole + 1, inspection.id);
            }
            node->setExpanded(true);
        }
        project->setExpanded(true);
    }

    void editFacility(bool create) {
        if (projectDb_.isEmpty()) return;
        const int row = facilityTable_->currentRow();
        if (!create && (row < 0 || row >= facilities_.size())) return;
        const FacilityInfo before = create ? FacilityInfo{} : facilities_[row];
        QDialog dialog(this);
        dialog.setWindowTitle(create ? QStringLiteral("시설물 추가") : QStringLiteral("시설물 편집"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *form = new QFormLayout;
        auto *name = new QLineEdit(before.name);
        auto *type = new QLineEdit(before.type);
        auto *address = new QLineEdit(before.address);
        auto *code = new QLineEdit(before.managementCode);
        auto *description = new QLineEdit(before.description);
        QString currentModel, error;
        if (!create) ProjectStore::facilityModelPath(projectDb_, before.id, &currentModel, &error);
        auto *model = new QLineEdit(currentModel);
        auto *modelBrowse = new QPushButton(QStringLiteral("찾아보기"));
        auto *modelRow = new QWidget;
        auto *modelLayout = new QHBoxLayout(modelRow);
        modelLayout->setContentsMargins(0, 0, 0, 0);
        modelLayout->addWidget(model);
        modelLayout->addWidget(modelBrowse);
        form->addRow(QStringLiteral("시설물명"), name);
        form->addRow(QStringLiteral("종류"), type);
        form->addRow(QStringLiteral("주소"), address);
        form->addRow(QStringLiteral("관리번호"), code);
        form->addRow(QStringLiteral("설명"), description);
        form->addRow(QStringLiteral("3D 모델 폴더 (선택)"), modelRow);
        layout->addLayout(form);
        connect(modelBrowse, &QPushButton::clicked, &dialog, [&] {
            const auto selected = QFileDialog::getExistingDirectory(&dialog, QStringLiteral("3D 모델 폴더 선택"), model->text());
            if (!selected.isEmpty()) model->setText(selected);
        });
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            FacilityInfo after = before;
            after.name = name->text(); after.type = type->text(); after.address = address->text();
            after.managementCode = code->text(); after.description = description->text();
            QString error, createdId;
            if (!model->text().trimmed().isEmpty() && !QFileInfo(model->text().trimmed()).isDir()) {
                QMessageBox::warning(&dialog, QStringLiteral("모델 경로 오류"), QStringLiteral("존재하는 모델 폴더를 선택하십시오.")); return;
            }
            const bool ok = create ? ProjectStore::addFacility(projectDb_, after, &error, &createdId)
                                   : ProjectStore::updateFacility(projectDb_, before, after, &error);
            if (!ok) { QMessageBox::warning(&dialog, QStringLiteral("저장 실패"), error); return; }
            if (!ProjectStore::setFacilityModelPath(projectDb_, create ? createdId : before.id, model->text().trimmed(), &error)) {
                QMessageBox::warning(&dialog, QStringLiteral("모델 경로 저장 실패"), error); return;
            }
            dialog.accept();
            refreshFacilities();
        });
        dialog.exec();
    }

    void editInspection(bool create) {
        if (selectedFacilityId_.isEmpty()) return;
        const int row = inspectionTable_->currentRow();
        if (!create && (row < 0 || row >= inspections_.size())) return;
        const InspectionInfo before = create ? InspectionInfo{} : inspections_[row];
        QDialog dialog(this);
        dialog.setWindowTitle(create ? QStringLiteral("점검 추가") : QStringLiteral("점검 편집"));
        auto *layout = new QVBoxLayout(&dialog);
        auto *form = new QFormLayout;
        auto *name = new QLineEdit(before.name);
        auto *type = new QLineEdit(before.type);
        auto *start = new QLineEdit(before.startedAt);
        auto *end = new QLineEdit(before.endedAt);
        auto *organization = new QLineEdit(before.organization);
        auto *inspector = new QLineEdit(before.inspector);
        QString currentCloud, error;
        if (!create) ProjectStore::inspectionCloudPath(projectDb_, before.id, &currentCloud, &error);
        auto *cloud = new QLineEdit(currentCloud);
        auto *cloudBrowse = new QPushButton(QStringLiteral("찾아보기"));
        auto *cloudRow = new QWidget;
        auto *cloudLayout = new QHBoxLayout(cloudRow);
        cloudLayout->setContentsMargins(0, 0, 0, 0);
        cloudLayout->addWidget(cloud);
        cloudLayout->addWidget(cloudBrowse);
        start->setPlaceholderText(QStringLiteral("YYYY-MM-DD"));
        end->setPlaceholderText(QStringLiteral("YYYY-MM-DD"));
        form->addRow(QStringLiteral("점검명"), name);
        form->addRow(QStringLiteral("종류"), type);
        form->addRow(QStringLiteral("시작일"), start);
        form->addRow(QStringLiteral("종료일"), end);
        form->addRow(QStringLiteral("수행기관"), organization);
        form->addRow(QStringLiteral("점검자"), inspector);
        form->addRow(QStringLiteral("LiDAR LAS (선택)"), cloudRow);
        layout->addLayout(form);
        connect(cloudBrowse, &QPushButton::clicked, &dialog, [&] {
            const auto selected = QFileDialog::getOpenFileName(&dialog, QStringLiteral("LiDAR LAS 선택"), cloud->text(), QStringLiteral("LAS (*.las)"));
            if (!selected.isEmpty()) cloud->setText(selected);
        });
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
            InspectionInfo after = before;
            if (create) after.facilityId = selectedFacilityId_;
            after.name = name->text(); after.type = type->text();
            after.startedAt = start->text().trimmed(); after.endedAt = end->text().trimmed();
            after.organization = organization->text(); after.inspector = inspector->text();
            QString error, createdId;
            const QFileInfo cloudFile(cloud->text().trimmed());
            if (!cloud->text().trimmed().isEmpty() && (!cloudFile.isFile() || cloudFile.suffix().compare(QStringLiteral("las"), Qt::CaseInsensitive))) {
                QMessageBox::warning(&dialog, QStringLiteral("LAS 경로 오류"), QStringLiteral("존재하는 LAS 파일을 선택하십시오.")); return;
            }
            const bool ok = create ? ProjectStore::addInspection(projectDb_, after, &error, &createdId)
                                   : ProjectStore::updateInspection(projectDb_, before, after, &error);
            if (!ok) { QMessageBox::warning(&dialog, QStringLiteral("저장 실패"), error); return; }
            if (!ProjectStore::setInspectionCloudPath(projectDb_, create ? createdId : before.id, cloud->text().trimmed(), &error)) {
                QMessageBox::warning(&dialog, QStringLiteral("LAS 경로 저장 실패"), error); return;
            }
            dialog.accept();
            selectFacility(facilityTable_->currentRow());
        });
        dialog.exec();
    }

    QString projectDb_;
    QString selectedFacilityId_;
    QString selectedInspectionId_;
    QList<FacilityInfo> facilities_;
    QList<InspectionInfo> inspections_;
    QLabel *scope_ = nullptr;
    QLabel *projectName_ = nullptr;
    QLabel *projectId_ = nullptr;
    QLabel *projectPath_ = nullptr;
    QLabel *inputDetail_ = nullptr;
    QLabel *datasetsLabel_ = nullptr;
    QTreeWidget *tree_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QTableWidget *recent_ = nullptr;
    QTableWidget *facilityTable_ = nullptr;
    QTableWidget *inspectionTable_ = nullptr;
    QList<QPushButton *> programButtons_;
    QTableWidget *workTable_ = nullptr;
    QPushButton *openCandidate_ = nullptr, *confirmCandidate_ = nullptr;
    QList<RegistrationReviewInfo> pendingReviews_;
    QTableWidget *pipelineTable_ = nullptr;
    QTableWidget *runTable_ = nullptr;
    QTableWidget *artifactTable_ = nullptr;
    QPushButton *runAll_ = nullptr;
    QLabel *pipelineNote_ = nullptr;
    QLabel *selectionDetail_ = nullptr;
    QLabel *status_ = nullptr;
    bool loadingModes_ = false;
};
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    ProjectWindow window;
    window.show();
    if (app.arguments().size() == 3 && app.arguments()[1] == QStringLiteral("--capture-ui"))
        QTimer::singleShot(100, &window, [&] {
            window.grab().save(app.arguments()[2]);
            app.quit();
        });
    return app.exec();
}
