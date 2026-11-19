#pragma once

#include <QString>

class ProjectArchive {
public:
    static bool backupProject(const QString &databasePath, const QString &emptyDestination, QString *error);
    static bool restoreProject(const QString &archiveDirectory, const QString &emptyDestination, QString *error);
};
