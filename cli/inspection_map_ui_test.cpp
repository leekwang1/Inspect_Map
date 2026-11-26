#define main inspection_map_editor_entry
#include "../apps/inspection_map_editor.cpp"
#undef main

#include <QTemporaryDir>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    if (argc < 2) return 1;
    const QString sourceDb = QString::fromLocal8Bit(argv[1]);
    QTemporaryDir temp;
    if (!temp.isValid()) return 2;
    const QString dbPath = QDir(temp.path()).filePath(QStringLiteral("project.sqlite"));
    if (!QFile::copy(sourceDb, dbPath)) return 3;
    const QDir calibrationSource(QFileInfo(sourceDb).dir().filePath(QStringLiteral("source/calibration")));
    const QString calibrationTarget = QDir(temp.path()).filePath(QStringLiteral("source/calibration"));
    if (!QDir().mkpath(calibrationTarget)) return 4;
    for (const auto &name : calibrationSource.entryList({QStringLiteral("*.json")}, QDir::Files))
        if (!QFile::copy(calibrationSource.filePath(name), QDir(calibrationTarget).filePath(name))) return 5;
    if (!qEnvironmentVariable("INSPECTMAP_TEST_OUTPUT").isEmpty()) {
        const QString connection = QStringLiteral("map-image-fixture");
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(dbPath);
            if (!db.open()) return 30;
            QSqlQuery query(db);
            if (!query.exec(QStringLiteral("SELECT COALESCE((SELECT v.storage_path FROM image_variant v "
                "WHERE v.image_id=i.image_id AND v.variant_type='DEVICE_UNDISTORTED' "
                "AND v.status='VALID' AND v.is_active=1 ORDER BY v.rowid DESC LIMIT 1),i.raw_path) "
                "FROM image i WHERE COALESCE(NULLIF(i.display_name,''),i.source_identifier) "
                "IN ('406_0239.JPG','406_0257.JPG')"))) return 31;
            while (query.next()) {
                const QString relative = query.value(0).toString();
                const QString source = QFileInfo(sourceDb).dir().filePath(relative);
                const QString target = QDir(temp.path()).filePath(relative);
                if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !QFile::copy(source, target)) return 32;
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }
    MapWindow window(dbPath, {});
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    const auto parts = window.findChildren<QListWidget *>();
    if (parts.size() < 2 || parts[0]->count() < 26 || parts[1]->count() == 0) return 6;
    window.runForBatch();
    const QString pdf = QDir(temp.path()).filePath(QStringLiteral("map-draft.pdf"));
    window.exportForBatch(QStringLiteral("SP-PY3-LSH-R04"), pdf);
    if (QFileInfo(pdf).size() < 1000) return 7;
    QString inspectionId, datasetId;
    {
        const QString connection = QStringLiteral("map-ui-test");
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(dbPath);
            if (!db.open()) return 8;
            QSqlQuery query(db);
            if (!query.exec(QStringLiteral("SELECT inspection_id,dataset_id FROM map_projection_run "
                    "WHERE active=1 LIMIT 1")) || !query.next()) return 9;
            inspectionId = query.value(0).toString(); datasetId = query.value(1).toString();
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }
    ProjectionBatch batch;
    QString error;
    if (!InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &batch, &error) ||
        batch.items.isEmpty()) return 10;
    const auto scenes = window.findChildren<QGraphicsScene *>();
    if (scenes.isEmpty() || scenes.first()->items().isEmpty()) return 11;
    MapDrawing generic; generic.partId = QStringLiteral("GENERIC"); generic.id = QStringLiteral("GENERIC-FR");
    if (drawingSide(generic) != QStringLiteral("SINGLE")) return 12;
    window.setMode(QStringLiteral("unroll"));
    if (parts[0]->count() < 2) return 13;
    int unrollRow = -1;
    for (int i = 0; i < parts[0]->count(); ++i)
        if (parts[0]->item(i)->data(Qt::UserRole).toString() == QStringLiteral("SP-PY3-LSH|IN"))
            unrollRow = i;
    if (unrollRow < 0) return 45;
    parts[0]->setCurrentRow(unrollRow);
    if (parts[1]->count() != 1 || scenes.first()->items().isEmpty()) return 14;
    const auto unrollTables = window.findChildren<QTableWidget *>();
    if (unrollTables.isEmpty()) return 42;
    if (unrollTables.first()->columnCount() != 7 ||
        unrollTables.first()->horizontalHeaderItem(2)->text() != QStringLiteral("폭(m)") ||
        unrollTables.first()->horizontalHeaderItem(4)->text() != QStringLiteral("두께(mm)") ||
        unrollTables.first()->horizontalHeaderItem(5)->text() != QStringLiteral("이미지명")) return 46;
    int previousNumber = 0;
    bool reachedUnnumbered = false;
    for (int row = 0; row < unrollTables.first()->rowCount(); ++row) {
        const QString label = unrollTables.first()->item(row, 0)->text();
        if (label == QStringLiteral("—")) {
            reachedUnnumbered = true;
            continue;
        }
        bool numeric = false;
        const int number = label.toInt(&numeric);
        if (!numeric || reachedUnnumbered || number <= previousNumber) return 43;
        previousNumber = number;
    }
    if (previousNumber < 10) return 44;
    InspectionMapEngine engine;
    if (!engine.load(QFileInfo(sourceDb).dir().filePath(QStringLiteral("../Data/model")), &error)) return 15;
    const auto *referenceDrawing = engine.drawing(QStringLiteral("SP-PY3-LSH-R04-IN-FR"));
    if (!referenceDrawing || referenceDrawing->lines.isEmpty()) return 49;
    QGraphicsScene referenceOnly;
    addDrawingAt(&referenceOnly, *referenceDrawing, batch, {}, {}, false);
    if (referenceOnly.items().size() != referenceDrawing->lines.size()) return 50;
    const auto *sheet = engine.unrollSheet(QStringLiteral("SP-PY3-LSH|IN"));
    if (!sheet) return 16;
    const auto numbered = numberedDamages(batch, {}, sheet);
    if (!numbered.isEmpty()) {
        const auto &damage = *numbered.first();
        if (unrollTables.first()->item(0, 2)->text() != metreLabel(damage.boxWidthMm) ||
            unrollTables.first()->item(0, 3)->text() != metreLabel(damage.boxLengthMm) ||
            unrollTables.first()->item(0, 4)->text() != dimensionLabel(damage.crackP90Mm) ||
            unrollTables.first()->item(0, 5)->text() != damage.imageName) return 48;
    }
    int candidates = 0, mapped = 0;
    for (const auto &damage : batch.items) {
        if (damage.status != QStringLiteral("PROJECTED") ||
            damage.faceId.section(QLatin1Char('-'), 4, 4) != QStringLiteral("IN") ||
            !sheet->parts.contains(damage.partId)) continue;
        ++candidates;
        bool valid = !damage.paths.isEmpty();
        for (const auto &path : damage.paths) for (const auto &point : path) {
            QPointF mappedPoint;
            if (!sheet->point(damage.faceId, point, &mappedPoint)) valid = false;
        }
        if (valid) ++mapped;
    }
    if (candidates < 10 || mapped < candidates * 9 / 10) return 17;
    const QString unrollPdf = QDir(temp.path()).filePath(QStringLiteral("unroll-draft.pdf"));
    window.exportForBatch(QStringLiteral("SP-PY3-LSH|IN"), unrollPdf);
    if (QFileInfo(unrollPdf).size() < 1000) return 18;
    const QString html = QDir(temp.path()).filePath(QStringLiteral("unroll.html"));
    const QString excel = QDir(temp.path()).filePath(QStringLiteral("quantity.xlsx"));
    if (!window.exportHtmlForBatch(QStringLiteral("SP-PY3-LSH|IN"), html) ||
        !window.exportXlsxForBatch(QStringLiteral("SP-PY3-LSH|IN"), excel)) return 19;
    QFile htmlFile(html), excelFile(excel);
    if (!htmlFile.open(QIODevice::ReadOnly) || !excelFile.open(QIODevice::ReadOnly)) return 20;
    const QByteArray htmlData = htmlFile.readAll(), excelData = excelFile.readAll();
    if (!htmlData.contains("<aside><h2>") || !htmlData.contains("grid-template-columns:") ||
        htmlData.contains("<th>부재</th>") || htmlData.contains("<th>영상</th>") ||
        !htmlData.contains("<th>폭</th><th>길이</th><th>단위</th>") ||
        htmlData.contains("면적(mm") || !htmlData.contains("data:image/png;base64,") ||
        htmlData.count("<tr>") != numberedDamages(batch, {}, sheet).size()+1 ||
        excelData.size() < 1000 || !excelData.startsWith("PK")) return 21;
    const QString output = qEnvironmentVariable("INSPECTMAP_TEST_OUTPUT");
    if (!output.isEmpty()) {
        if (!QDir().mkpath(output) ||
            !window.grab().save(QDir(output).filePath(QStringLiteral("unroll-ui.png")))) return 47;
        window.setMode(QStringLiteral("map"));
        for (int i = 0; i < parts[0]->count(); ++i)
            if (parts[0]->item(i)->data(Qt::UserRole).toString() == QStringLiteral("__REVIEW__")) {
                parts[0]->setCurrentRow(i); break;
            }
        const auto tables = window.findChildren<QTableWidget *>();
        if (!tables.isEmpty() && tables.first()->rowCount() > 0) tables.first()->selectRow(0);
        if (!QDir().mkpath(output) ||
            !QFile::copy(unrollPdf, QDir(output).filePath(QStringLiteral("unroll.pdf"))) ||
            !QFile::copy(html, QDir(output).filePath(QStringLiteral("unroll.html"))) ||
            !QFile::copy(excel, QDir(output).filePath(QStringLiteral("quantity.xlsx"))) ||
            !window.grab().save(QDir(output).filePath(QStringLiteral("review-ui.png")))) return 22;
    }
    window.setMode(QStringLiteral("map"));
    int reviewRow = -1;
    for (int i = 0; i < parts[0]->count(); ++i)
        if (parts[0]->item(i)->data(Qt::UserRole).toString() == QStringLiteral("__REVIEW__")) reviewRow = i;
    if (reviewRow < 0) {
        const bool unresolved = std::any_of(batch.items.cbegin(), batch.items.cend(), [](const auto &item) {
            return item.status != QStringLiteral("PROJECTED") && item.status != QStringLiteral("EXCLUDED");
        });
        return unresolved ? 23 : 0;
    }
    parts[0]->setCurrentRow(reviewRow);
    const auto tables = window.findChildren<QTableWidget *>();
    if (tables.isEmpty() || tables.first()->rowCount() == 0) return 24;
    tables.first()->selectRow(0);
    const QString objectId = tables.first()->item(0, 0)->data(Qt::UserRole).toString();
    QPushButton *exclude = nullptr, *reset = nullptr;
    for (auto *button : window.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("제외")) exclude = button;
        if (button->text() == QStringLiteral("되돌리기")) reset = button;
    }
    if (!exclude || !reset || objectId.isEmpty()) return 25;
    exclude->click();
    ProjectionBatch reviewed;
    if (!InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &reviewed, &error)) return 26;
    bool excluded = false;
    for (const auto &item : reviewed.items) if (item.objectId == objectId)
        excluded = item.status == QStringLiteral("EXCLUDED");
    if (!excluded) return 27;
    reset->click();
    if (!InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &reviewed, &error)) return 28;
    for (const auto &item : reviewed.items) if (item.objectId == objectId &&
        !item.reviewState.isEmpty()) return 29;
    const auto hit = engine.cast(QVector3D(3, -5, 22.649f), QVector3D(0, 1, 0));
    const auto *target = engine.drawingForFace(hit.faceId);
    if (!hit.found || !target) return 33;
    QComboBox *partCombo = nullptr, *drawingCombo = nullptr, *faceCombo = nullptr;
    for (auto *combo : window.findChildren<QComboBox *>()) {
        if (combo->toolTip() == QStringLiteral("수동 교정 대상 부재")) partCombo = combo;
        if (combo->toolTip() == QStringLiteral("수동 교정 대상 방향도")) drawingCombo = combo;
        if (combo->toolTip() == QStringLiteral("손상이 놓인 모델 면")) faceCombo = combo;
    }
    if (!partCombo || !drawingCombo || !faceCombo) return 34;
    if (partCombo->findData(hit.partId) < 0) return 39;
    partCombo->setCurrentIndex(partCombo->findData(hit.partId));
    if (drawingCombo->findData(target->id) < 0) return 40;
    drawingCombo->setCurrentIndex(drawingCombo->findData(target->id));
    if (faceCombo->findData(hit.faceId) < 0) return 41;
    faceCombo->setCurrentIndex(faceCombo->findData(hit.faceId));
    for (auto *combo : window.findChildren<QComboBox *>())
        if (combo->findData(QStringLiteral("LINE")) >= 0)
            combo->setCurrentIndex(combo->findData(QStringLiteral("LINE")));
    QPushButton *start = nullptr, *confirm = nullptr;
    for (auto *button : window.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("도면에 점 찍기")) start = button;
        if (button->text() == QStringLiteral("위치·물량 확정")) confirm = button;
    }
    const auto views = window.findChildren<QGraphicsView *>();
    if (!start || !confirm || views.isEmpty()) return 35;
    const QPointF flat = InspectionMapEngine::drawingPoint(*target, hit.point);
    QVector3D secondPoint;
    if (!engine.mapPoint(target->id, hit.faceId, flat+QPointF(0.1, 0), &secondPoint)) return 36;
    start->click();
    auto *drawingView = static_cast<DrawingView *>(views.first());
    drawingView->pointPicked(flat);
    drawingView->pointPicked(flat+QPointF(0.1, 0));
    confirm->click();
    if (!InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &reviewed, &error)) return 37;
    bool corrected = false;
    for (const auto &item : reviewed.items) if (item.objectId == objectId)
        corrected = item.status == QStringLiteral("PROJECTED") &&
            item.reviewState == QStringLiteral("CORRECTED") && item.lengthMm > 0;
    if (!corrected) return 38;
    return 0;
}
