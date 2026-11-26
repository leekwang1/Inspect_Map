#define main inspection_map_editor_entry
#include "../apps/inspection_map_editor.cpp"
#undef main

#include <QTemporaryDir>
#include <QSet>

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
    if (unrollTables.first()->selectionMode() != QAbstractItemView::ExtendedSelection) return 51;
    auto *damageTable = unrollTables.first();
    auto *unrollView = dynamic_cast<DrawingView *>(window.findChild<QGraphicsView *>());
    if (!unrollView || damageTable->rowCount() < 2) return 53;
    const QString firstId = damageTable->item(0, 0)->data(Qt::UserRole).toString();
    const QString secondId = damageTable->item(1, 0)->data(Qt::UserRole).toString();
    if (firstId.isEmpty() || secondId.isEmpty() || firstId == secondId) return 54;
    for (int row : {0, 1})
        damageTable->selectionModel()->select(damageTable->model()->index(row, 0),
            QItemSelectionModel::Select | QItemSelectionModel::Rows);
    if (damageTable->selectionModel()->selectedRows().size() != 2) return 55;
    QSet<QString> highlighted;
    QRectF area;
    for (const auto *item : scenes.first()->items()) {
        const auto *path = dynamic_cast<const QGraphicsPathItem *>(item);
        if (!path || path->pen().color() != QColor(0, 141, 112)) continue;
        const QString id = item->data(1).toString();
        if (id == firstId || id == secondId) {
            highlighted.insert(id);
            const QRectF bounds = unrollView->viewportTransform().mapRect(item->sceneBoundingRect());
            area = area.isNull() ? bounds : area.united(bounds);
        }
    }
    if (highlighted.size() != 2 || !unrollView->areaPicked) return 56;
    damageTable->clearSelection();
    unrollView->areaPicked(area.adjusted(-4, -4, 4, 4).toAlignedRect());
    QSet<QString> areaSelected;
    for (const auto &index : damageTable->selectionModel()->selectedRows())
        areaSelected.insert(damageTable->item(index.row(), 0)->data(Qt::UserRole).toString());
    if (!areaSelected.contains(firstId) || !areaSelected.contains(secondId)) return 57;
    QGraphicsScene probeScene;
    QPainterPath separatedStrokes;
    separatedStrokes.moveTo(0, 0); separatedStrokes.lineTo(0, 100);
    separatedStrokes.moveTo(100, 0); separatedStrokes.lineTo(100, 100);
    probeScene.addPath(separatedStrokes, QPen(Qt::red))->setData(1, QStringLiteral("probe"));
    QGraphicsView probeView(&probeScene);
    const QPoint emptyMiddle = probeView.mapFromScene(QPointF(50, 50));
    if (!damageIdsInArea(&probeScene, &probeView, QRect(emptyMiddle-QPoint(5, 5), QSize(10, 10))).isEmpty())
        return 58;
    unrollView->areaPicked(QRect(-100, -100, 10, 10));
    if (!damageTable->selectionModel()->selectedRows().isEmpty()) return 59;
    damageTable->selectRow(0);
    if (!unrollView->selectionCleared) return 60;
    QMouseEvent rightClick(QEvent::MouseButtonPress, QPointF(10, 10),
        Qt::RightButton, Qt::RightButton, Qt::NoModifier);
    QApplication::sendEvent(unrollView->viewport(), &rightClick);
    if (!damageTable->selectionModel()->selectedRows().isEmpty()) return 61;
    QSet<QString> groupCommands;
    for (const auto *button : window.findChildren<QPushButton *>()) groupCommands.insert(button->text());
    if (!groupCommands.contains(QStringLiteral("동일 손상")) ||
        !groupCommands.contains(QStringLiteral("이어짐")) ||
        !groupCommands.contains(QStringLiteral("묶음 해제"))) return 52;
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
    QPushButton *defer = nullptr;
    for (auto *button : window.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("보류")) defer = button;
        if (button->text() == QStringLiteral("도면에 점 찍기") ||
            button->text() == QStringLiteral("위치·물량 확정")) return 33;
    }
    if (!defer) return 34;
    defer->click();
    if (!InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &reviewed, &error)) return 35;
    bool deferred = false;
    for (const auto &item : reviewed.items) if (item.objectId == objectId)
        deferred = item.reviewState == QStringLiteral("DEFERRED");
    if (!deferred) return 36;
    reset->click();
    if (!InspectionMapWorkflow::active(dbPath, inspectionId, datasetId, &reviewed, &error)) return 37;
    for (const auto &item : reviewed.items) if (item.objectId == objectId &&
        !item.reviewState.isEmpty()) return 38;
    return 0;
}
