#define main ai_damage_manager_entry
#include "../apps/ai_damage_manager.cpp"
#undef main

#include <QTemporaryDir>
#include <QSqlDatabase>
#include <QSqlQuery>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    if (argc < 3) return 1;
    const QString sourceDb = QString::fromLocal8Bit(argv[1]);
    const QString jsonFolder = QString::fromLocal8Bit(argv[2]);
    QTemporaryDir temp;
    if (!temp.isValid()) return 2;
    const QString dbPath = QDir(temp.path()).filePath(QStringLiteral("project.sqlite"));
    if (!QFile::copy(sourceDb, dbPath)) return 3;
    {
        const QString connection = QStringLiteral("ui-smoke");
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(dbPath);
            if (!db.open()) return 4;
            const QDir sourceDir = QFileInfo(sourceDb).absoluteDir();
            for (const auto &entry : {qMakePair(QStringLiteral("image"), QStringLiteral("raw_path")),
                                      qMakePair(QStringLiteral("image_variant"), QStringLiteral("storage_path"))}) {
                QSqlQuery rows(db);
                if (!rows.exec(QStringLiteral("SELECT rowid,%1 FROM %2").arg(entry.second, entry.first))) return 5;
                QList<QPair<qint64, QString>> paths;
                while (rows.next()) {
                    const QString path = rows.value(1).toString();
                    if (!QFileInfo(path).isAbsolute()) paths.append({rows.value(0).toLongLong(), sourceDir.filePath(path)});
                }
                rows.finish();
                QSqlQuery update(db);
                if (!update.prepare(QStringLiteral("UPDATE %1 SET %2=? WHERE rowid=?")
                    .arg(entry.first, entry.second))) return 6;
                for (const auto &path : paths) {
                    update.bindValue(0, path.second);
                    update.bindValue(1, path.first);
                    if (!update.exec()) return 7;
                }
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }
    QString error;
    QList<DamageDataset> datasets;
    if (!DamageStore::datasets(dbPath, {}, &datasets, &error) || datasets.isEmpty()) return 8;
    DamageImportReport report;
    if (!DamageStore::importFolder(dbPath, datasets.first().id, jsonFolder, &report, &error) || report.images < 2)
        return 9;

    DamageWindow window(dbPath, {});
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.show();
    QTimer::singleShot(0, &window, [&] {
        auto tables = window.findChildren<QTableWidget *>();
        QTableWidget *images = nullptr, *objects = nullptr;
        for (auto *table : tables) {
            if (table->columnCount() == 2) images = table;
            if (table->columnCount() == 4) objects = table;
        }
        QComboBox *review = nullptr;
        for (auto *combo : window.findChildren<QComboBox *>())
            if (combo->findData(QStringLiteral("REJECTED")) >= 0) review = combo;
        QPushButton *save = nullptr;
        for (auto *button : window.findChildren<QPushButton *>())
            if (button->text().startsWith(QStringLiteral("현재 영상 검토 저장"))) save = button;
        if (!images || !objects || !review || !save || images->rowCount() < 2 || objects->rowCount() < 2) {
            app.exit(10); return;
        }
        objects->selectionModel()->select(objects->model()->index(1, 0),
            QItemSelectionModel::Select | QItemSelectionModel::Rows);
        objects->selectionModel()->setCurrentIndex(objects->model()->index(1, 0), QItemSelectionModel::NoUpdate);
        if (objects->selectionModel()->selectedRows().size() != 2) { app.exit(11); return; }
        review->setCurrentIndex(review->findData(QStringLiteral("ACCEPTED")));
        if (!save->isEnabled() || !objects->item(0, 3)->text().startsWith(QStringLiteral("채택")) ||
            !objects->item(1, 3)->text().startsWith(QStringLiteral("채택"))) { app.exit(12); return; }
        save->click();
        images->setCurrentCell(1, 0);
        if (images->currentRow() != 1 || objects->rowCount() == 0) { app.exit(13); return; }
        images->setCurrentCell(0, 0);
        if (objects->rowCount() < 2 || objects->item(0, 3)->text() != QStringLiteral("채택") ||
            objects->item(1, 3)->text() != QStringLiteral("채택")) { app.exit(14); return; }
        for (int i = 0; i < 5; ++i) {
            images->setCurrentCell(1, 0);
            images->setCurrentCell(0, 0);
            if (objects->rowCount() < 2 || objects->item(0, 3)->text() != QStringLiteral("채택")) {
                app.exit(15); return;
            }
        }
        auto *view = static_cast<ImageView *>(window.findChild<QGraphicsView *>());
        if (!view || view->viewport()->width() < 20 || view->viewport()->height() < 20) {
            app.exit(16); return;
        }
        const QRect area = view->viewport()->rect().adjusted(2, 2, -2, -2);
        const auto sendMouse = [view](QEvent::Type type, const QPoint &point,
                                      Qt::MouseButton button, Qt::MouseButtons buttons,
                                      Qt::KeyboardModifiers modifiers = Qt::ControlModifier) {
            QMouseEvent event(type, QPointF(point), button, buttons, modifiers);
            QApplication::sendEvent(view->viewport(), &event);
        };
        sendMouse(QEvent::MouseButtonPress, area.topLeft(), Qt::LeftButton, Qt::LeftButton);
        sendMouse(QEvent::MouseMove, area.bottomRight(), Qt::NoButton, Qt::LeftButton);
        sendMouse(QEvent::MouseButtonRelease, area.bottomRight(), Qt::LeftButton, Qt::NoButton);
        QSet<int> selectedInScene;
        for (auto *item : view->scene()->selectedItems()) selectedInScene.insert(item->data(0).toInt());
        if (objects->selectionModel()->selectedRows().size() < 2 ||
            selectedInScene.size() != objects->selectionModel()->selectedRows().size() ||
            view->dragMode() != QGraphicsView::ScrollHandDrag) { app.exit(17); return; }
        QPoint emptyPoint;
        bool foundEmpty = false;
        for (int y = 4; y < view->viewport()->height() && !foundEmpty; y += 20) {
            for (int x = 4; x < view->viewport()->width(); x += 20) {
                const auto hits = view->scene()->items(view->mapToScene(QPoint(x, y)),
                    Qt::IntersectsItemShape, Qt::DescendingOrder, view->viewportTransform());
                if (std::none_of(hits.cbegin(), hits.cend(), [](const auto *item) {
                    return item->flags() & QGraphicsItem::ItemIsSelectable;
                })) { emptyPoint = QPoint(x, y); foundEmpty = true; break; }
            }
        }
        if (!foundEmpty) { app.exit(18); return; }
        sendMouse(QEvent::MouseButtonPress, emptyPoint, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        sendMouse(QEvent::MouseButtonRelease, emptyPoint, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        if (objects->selectionModel()->selectedRows().size() != selectedInScene.size() ||
            view->scene()->selectedItems().isEmpty()) { app.exit(19); return; }
        sendMouse(QEvent::MouseButtonPress, area.center(), Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        sendMouse(QEvent::MouseButtonRelease, area.center(), Qt::RightButton, Qt::NoButton, Qt::NoModifier);
        if (!objects->selectionModel()->selectedRows().isEmpty() ||
            !view->scene()->selectedItems().isEmpty() || objects->currentRow() != -1) {
            app.exit(20); return;
        }
        app.exit(0);
    });
    return app.exec();
}
