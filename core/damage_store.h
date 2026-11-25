#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

struct DamageDataset {
    QString id;
    QString name;
    int imageCount = 0;
};

struct DamageImage {
    QString id;
    QString name;
    QString path;
    QString resultId;
    QString sourceJson;
    int width = 0;
    int height = 0;
    int objectCount = 0;
    double gsd = 0;
    int pendingReviewCount = 0;
};

struct DamageObject {
    QString id;
    QString sourceId;
    QString type;
    QString label;
    QJsonArray bbox;
    QJsonObject geometry;
    QJsonObject measurements;
    QJsonObject llmReview;
    QJsonArray flags;
    QString reviewState;
    QString overrideType;
    QString note;
    bool reviewConfirmed = false;
};

struct DamageImportReport {
    int images = 0;
    int objects = 0;
    QStringList warnings;
};

struct DamageReview {
    QString objectId;
    QString state;
    QString type;
    QString note;
};

class DamageStore {
public:
    static bool ensureSchema(const QString &projectDb, QString *error);
    static bool datasets(const QString &projectDb, const QString &inspectionId,
                         QList<DamageDataset> *items, QString *error);
    static bool images(const QString &projectDb, const QString &datasetId,
                       QList<DamageImage> *items, QString *error);
    static bool objects(const QString &projectDb, const QString &resultId,
                        QList<DamageObject> *items, QString *error);
    static bool importFolder(const QString &projectDb, const QString &datasetId,
                             const QString &folder, DamageImportReport *report, QString *error);
    static bool setReview(const QString &projectDb, const QString &objectId,
                          const QString &state, const QString &type, const QString &note, QString *error);
    static bool setReviews(const QString &projectDb, const QString &resultId,
                           const QList<DamageReview> &reviews, QString *error);
    static bool addObject(const QString &projectDb, const QString &resultId,
                          const QString &type, const QJsonArray &bbox, QString *error);
};
