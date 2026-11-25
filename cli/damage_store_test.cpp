#include "conversion_core.h"
#include "damage_store.h"
#include "project_store.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    if (argc < 2) return 1;
    const QString folder = QString::fromLocal8Bit(argv[1]);
    const QStringList files = QDir(folder).entryList({QStringLiteral("*.json")}, QDir::Files);
    if (files.size() != 24) return 2;
    QTemporaryDir project;
    QString error;
    if (!project.isValid() || !ProjectStore::create(project.path(), QStringLiteral("Damage test"), &error)) return 3;
    const QString path = ProjectStore::databasePath(project.path());
    if (!ConversionCore::ensureSchema(path, &error) || !DamageStore::ensureSchema(path, &error)) return 4;
    const QString datasetId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        const QString connection = QStringLiteral("damage-fixture");
        {
            auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(path);
            if (!db.open()) return 5;
            QSqlQuery query(db);
            query.prepare(QStringLiteral("INSERT INTO dataset VALUES(?,?,?,?,?,?,?)"));
            for (const auto &value : QVariantList{datasetId, QVariant(), QStringLiteral("Elios"),
                 QStringLiteral("ELIOS"), folder, QStringLiteral("VALID"), QStringLiteral("2026-01-01")})
                query.addBindValue(value);
            if (!query.exec()) return 6;
            for (const auto &name : files) {
                QFile file(QDir(folder).filePath(name));
                if (!file.open(QIODevice::ReadOnly)) return 7;
                const auto image = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("image")).toObject();
                QString displayName = image.value(QStringLiteral("file")).toString();
                displayName.replace(QStringLiteral("_undistorted.jpg"), QStringLiteral(".JPG"), Qt::CaseInsensitive);
                query.prepare(QStringLiteral("INSERT INTO image "
                    "(image_id,dataset_id,source_identifier,raw_path,width,height,include_state,display_name) "
                    "VALUES(?,?,?,?,?,?,?,?)"));
                for (const auto &value : QVariantList{QUuid::createUuid().toString(QUuid::WithoutBraces),
                     datasetId, displayName, displayName,
                     image.value(QStringLiteral("width")).toInt(), image.value(QStringLiteral("height")).toInt(),
                     QStringLiteral("INCLUDED"), displayName}) query.addBindValue(value);
                if (!query.exec()) return 8;
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(connection);
    }
    DamageImportReport report;
    QList<DamageDataset> datasets;
    if (!DamageStore::datasets(path, {}, &datasets, &error) || datasets.size() != 1 ||
        datasets[0].imageCount != files.size()) return 16;
    if (!DamageStore::datasets(path, QStringLiteral("selected-inspection"), &datasets, &error) ||
        datasets.size() != 1 || datasets[0].id != datasetId) return 17;
    if (!DamageStore::importFolder(path, datasetId, folder, &report, &error) ||
        report.images != files.size() || report.objects < 500 || !report.warnings.isEmpty()) return 9;
    QList<DamageImage> images;
    if (!DamageStore::images(path, datasetId, &images, &error) || images.size() != files.size()) return 10;
    int accepted = 0, rejected = 0, targets = 0;
    for (const auto &image : images) {
        if (image.resultId.isEmpty() || image.objectCount == 0) return 11;
        QList<DamageObject> entries;
        if (!DamageStore::objects(path, image.resultId, &entries, &error)) return 20;
        int pending = 0;
        for (const auto &entry : entries) {
            if (entry.reviewConfirmed) return 21;
            if (entry.type.startsWith(QStringLiteral("target_"))) {
                if (entry.reviewState != QStringLiteral("UNREVIEWED")) return 22;
                ++targets;
            } else {
                const bool certain = entry.llmReview.value(QStringLiteral("label")).toString() == QStringLiteral("확실");
                if (entry.reviewState != (certain ? QStringLiteral("ACCEPTED") : QStringLiteral("REJECTED"))) return 23;
                certain ? ++accepted : ++rejected;
                ++pending;
            }
        }
        if (image.pendingReviewCount != pending) return 24;
    }
    if (accepted != 30 || rejected != 506 || targets != 43) return 25;
    QList<DamageObject> objects;
    if (!DamageStore::objects(path, images[0].resultId, &objects, &error) || objects.isEmpty()) return 12;
    const QString objectId = objects[0].id;
    if (!DamageStore::setReview(path, objectId, QStringLiteral("ACCEPTED"),
                                QStringLiteral("crack"), QStringLiteral("checked"), &error) ||
        !DamageStore::addObject(path, images[0].resultId, QStringLiteral("crack"),
                                QJsonArray{0, 0, 20, 30}, &error) ||
        !DamageStore::addObject(path, images[0].resultId, QStringLiteral("crack"),
                                QJsonArray{40, 40, 60, 70}, &error)) return 13;
    if (!DamageStore::objects(path, images[0].resultId, &objects, &error) ||
        objects[0].reviewState != QStringLiteral("ACCEPTED") ||
        objects[0].note != QStringLiteral("checked") || !objects[0].reviewConfirmed ||
        objects[objects.size() - 2].sourceId != QStringLiteral("manual-001") ||
        objects.last().sourceId != QStringLiteral("manual-002") ||
        objects.last().reviewState != QStringLiteral("ACCEPTED") ||
        !objects.last().reviewConfirmed) return 14;
    const QString secondId = objects[1].id;
    const QList<DamageReview> batch{{objectId, QStringLiteral("REJECTED"), {}, QStringLiteral("first")},
                                    {secondId, QStringLiteral("HOLD"), {}, QStringLiteral("second")}};
    if (!DamageStore::setReviews(path, images[0].resultId, batch, &error)) return 18;
    if (!DamageStore::objects(path, images[0].resultId, &objects, &error) ||
        objects[0].reviewState != QStringLiteral("REJECTED") ||
        objects[1].reviewState != QStringLiteral("HOLD") ||
        !objects[1].reviewConfirmed) {
        return 18;
    }
    error.clear();
    const QList<DamageReview> invalid{{objectId, QStringLiteral("ACCEPTED"), {}, {}},
                                      {QStringLiteral("missing-object"), QStringLiteral("ACCEPTED"), {}, {}}};
    if (DamageStore::setReviews(path, images[0].resultId, invalid, &error) ||
        !DamageStore::objects(path, images[0].resultId, &objects, &error) ||
        objects[0].reviewState != QStringLiteral("REJECTED")) return 19;
    const QString firstResult = images[0].resultId;
    if (!DamageStore::importFolder(path, datasetId, folder, &report, &error) ||
        report.images != files.size() || !DamageStore::images(path, datasetId, &images, &error) ||
        images[0].resultId == firstResult ||
        !DamageStore::objects(path, images[0].resultId, &objects, &error) ||
        objects[0].reviewState != (objects[0].llmReview.value(QStringLiteral("label")).toString() == QStringLiteral("확실") ?
            QStringLiteral("ACCEPTED") : QStringLiteral("REJECTED")) ||
        objects[0].reviewConfirmed) return 15;
    return 0;
}
