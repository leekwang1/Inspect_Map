#pragma once

#include <QString>
#include <QList>
#include <QMap>

struct ProjectInfo {
    QString id;
    QString name;
    QString description;
    QString createdAt;
};

struct ProjectSetup {
    QString facilityName;
    QString inspectionName;
    QString modelRoot;
    QString cloudPath;
};

struct FacilityInfo {
    QString id;
    QString name;
    QString type;
    QString address;
    QString managementCode;
    QString description;
};

struct InspectionInfo {
    QString id;
    QString facilityId;
    QString name;
    QString type;
    QString status;
    QString startedAt;
    QString endedAt;
    QString organization;
    QString inspector;
};

struct ModuleRunInfo {
    QString id;
    QString inspectionId;
    QString moduleId;
    QString status;
    QString startedAt;
    QString finishedAt;
    QString error;
};

struct ArtifactInfo {
    QString id;
    QString runId;
    QString type;
    QString path;
    QString status;
    QString createdAt;
    bool active = false;
};

class ProjectStore {
public:
    static bool create(const QString &directory, const QString &name, QString *error);
    static bool create(const QString &directory, const QString &name, const ProjectSetup &setup, QString *error);
    static bool read(const QString &databasePath, ProjectInfo *info, QString *error);
    static bool facilities(const QString &databasePath, QList<FacilityInfo> *items, QString *error);
    static bool inspections(const QString &databasePath, const QString &facilityId, QList<InspectionInfo> *items, QString *error);
    static bool addFacility(const QString &databasePath, const QString &name, const QString &type, QString *error);
    static bool addFacility(const QString &databasePath, const FacilityInfo &facility, QString *error, QString *createdId = nullptr);
    static bool addInspection(const QString &databasePath, const QString &facilityId, const QString &name, const QString &type, QString *error);
    static bool addInspection(const QString &databasePath, const InspectionInfo &inspection, QString *error, QString *createdId = nullptr);
    static bool facilityModelPath(const QString &databasePath, const QString &facilityId, QString *modelRoot, QString *error);
    static bool inspectionCloudPath(const QString &databasePath, const QString &inspectionId, QString *cloudPath, QString *error);
    static bool setFacilityModelPath(const QString &databasePath, const QString &facilityId, const QString &modelRoot, QString *error);
    static bool setInspectionCloudPath(const QString &databasePath, const QString &inspectionId, const QString &cloudPath, QString *error);
    static bool updateFacility(const QString &databasePath, const FacilityInfo &before, const FacilityInfo &after, QString *error);
    static bool updateInspection(const QString &databasePath, const InspectionInfo &before, const InspectionInfo &after, QString *error);
    static bool backup(const QString &databasePath, const QString &destination, QString *error);
    static bool integrityCheck(const QString &databasePath, QString *result, QString *error);
    static bool ensureManagerSchema(const QString &databasePath, QString *error);
    static bool pipelineModes(const QString &databasePath, QMap<QString, QString> *modes, QString *error);
    static bool setPipelineMode(const QString &databasePath, const QString &moduleId, const QString &mode, QString *error);
    static bool runs(const QString &databasePath, const QString &inspectionId, QList<ModuleRunInfo> *items, QString *error);
    static bool artifacts(const QString &databasePath, QList<ArtifactInfo> *items, QString *error);
    static bool recordRun(const QString &databasePath, const ModuleRunInfo &run, QString *error);
    static bool recordArtifact(const QString &databasePath, const ArtifactInfo &artifact, QString *error);
    static bool adoptArtifact(const QString &databasePath, const QString &artifactId, QString *error);
    static bool linkArtifacts(const QString &databasePath, const QString &parentId, const QString &childId, QString *error);
    static QString databasePath(const QString &directory);
};
