#include "project_archive.h"
#include "project_store.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {
constexpr auto manifestName = "inspectmap-backup.json";

bool emptyDirectory(const QString &path, QString *error) {
    const QDir dir(path);
    if (!dir.exists() || !dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
        if (error) *error = QStringLiteral("Destination must be an existing empty directory.");
        return false;
    }
    return true;
}

bool separateDirectories(const QString &source, const QString &destination, QString *error) {
    const auto a = QDir(source).canonicalPath().toLower() + QLatin1Char('/');
    const auto b = QDir(destination).canonicalPath().toLower() + QLatin1Char('/');
    if (a == b || a.startsWith(b) || b.startsWith(a)) {
        if (error) *error = QStringLiteral("Source and destination must not contain each other.");
        return false;
    }
    return true;
}

QByteArray digest(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const auto block = file.read(1024 * 1024);
        if (block.isEmpty() && file.error() != QFile::NoError) {
            if (error) *error = file.errorString();
            return {};
        }
        hash.addData(block);
    }
    return hash.result().toHex();
}

bool copyChecked(const QString &source, const QString &destination, QString *error) {
    if (!QDir().mkpath(QFileInfo(destination).absolutePath()) || !QFile::copy(source, destination)) {
        if (error) *error = QStringLiteral("Copy failed: %1").arg(source);
        return false;
    }
    const auto sourceHash = digest(source, error);
    const auto destinationHash = digest(destination, error);
    if (sourceHash.isEmpty() || sourceHash != destinationHash) {
        if (error && error->isEmpty()) *error = QStringLiteral("Copied file checksum differs: %1").arg(source);
        return false;
    }
    return true;
}

bool safeRelativePath(const QString &relative) {
    const auto clean = QDir::cleanPath(relative);
    return !relative.isEmpty() && !QDir::isAbsolutePath(relative) && clean == relative &&
           clean != QStringLiteral("..") && !clean.startsWith(QStringLiteral("../"));
}
}

bool ProjectArchive::backupProject(const QString &databasePath, const QString &emptyDestination, QString *error) {
    if (!emptyDirectory(emptyDestination, error)) return false;
    const auto root = QFileInfo(databasePath).absolutePath();
    if (!separateDirectories(root, emptyDestination, error)) return false;
    ProjectInfo info;
    if (!ProjectStore::read(databasePath, &info, error)) return false;
    const auto databaseCopy = QDir(emptyDestination).filePath(QStringLiteral("project.sqlite"));
    if (!ProjectStore::backup(databasePath, databaseCopy, error)) return false;

    QJsonArray files;
    QDirIterator iterator(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const auto source = iterator.next();
        const QFileInfo file(source);
        if (file.isSymLink()) {
            if (error) *error = QStringLiteral("Symlink is not supported in a project archive: %1").arg(source);
            return false;
        }
        if (file.isDir()) continue;
        const auto relative = QDir(root).relativeFilePath(source);
        if (relative == QStringLiteral("project.sqlite") || relative == QStringLiteral("project.sqlite-wal") ||
            relative == QStringLiteral("project.sqlite-shm") || relative == QLatin1String(manifestName)) continue;
        if (!safeRelativePath(relative)) {
            if (error) *error = QStringLiteral("Invalid project-relative path: %1").arg(relative);
            return false;
        }
        const auto target = QDir(emptyDestination).filePath(relative);
        if (!copyChecked(source, target, error)) return false;
        files.append(QJsonObject{{QStringLiteral("path"), relative},
                                 {QStringLiteral("sha256"), QString::fromLatin1(digest(target, error))},
                                 {QStringLiteral("size"), static_cast<double>(QFileInfo(target).size())}});
    }
    const auto dbHash = digest(databaseCopy, error);
    if (dbHash.isEmpty()) return false;
    QString integrity;
    if (!ProjectStore::integrityCheck(databaseCopy, &integrity, error)) {
        if (error && error->isEmpty()) *error = integrity;
        return false;
    }
    files.prepend(QJsonObject{{QStringLiteral("path"), QStringLiteral("project.sqlite")},
                              {QStringLiteral("sha256"), QString::fromLatin1(dbHash)},
                              {QStringLiteral("size"), static_cast<double>(QFileInfo(databaseCopy).size())}});
    QSaveFile manifest(QDir(emptyDestination).filePath(QLatin1String(manifestName)));
    if (!manifest.open(QIODevice::WriteOnly)) { if (error) *error = manifest.errorString(); return false; }
    const QJsonDocument document(QJsonObject{{QStringLiteral("format"), QStringLiteral("inspectmap.project.backup.v1")},
                                            {QStringLiteral("project_id"), info.id}, {QStringLiteral("files"), files}});
    if (manifest.write(document.toJson()) < 0 || !manifest.commit()) {
        if (error) *error = manifest.errorString();
        return false;
    }
    return true;
}

bool ProjectArchive::restoreProject(const QString &archiveDirectory, const QString &emptyDestination, QString *error) {
    if (!emptyDirectory(emptyDestination, error) || !separateDirectories(archiveDirectory, emptyDestination, error)) return false;
    QFile manifest(QDir(archiveDirectory).filePath(QLatin1String(manifestName)));
    if (!manifest.open(QIODevice::ReadOnly)) { if (error) *error = manifest.errorString(); return false; }
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(manifest.readAll(), &parseError);
    const auto object = document.object();
    if (parseError.error != QJsonParseError::NoError || object.value(QStringLiteral("format")).toString() !=
        QStringLiteral("inspectmap.project.backup.v1") || !object.value(QStringLiteral("files")).isArray()) {
        if (error) *error = QStringLiteral("Invalid InspectMap backup manifest.");
        return false;
    }
    const auto files = object.value(QStringLiteral("files")).toArray();
    bool hasDatabase = false;
    for (const auto &entry : files) {
        const auto record = entry.toObject();
        const auto relative = record.value(QStringLiteral("path")).toString();
        if (!safeRelativePath(relative) || relative == QLatin1String(manifestName)) {
            if (error) *error = QStringLiteral("Unsafe backup path.");
            return false;
        }
        if (relative == QStringLiteral("project.sqlite")) hasDatabase = true;
        const auto source = QDir(archiveDirectory).filePath(relative);
        const QFileInfo file(source);
        if (!file.isFile() || file.isSymLink() || QString::fromLatin1(digest(source, error)) !=
            record.value(QStringLiteral("sha256")).toString() || file.size() !=
            static_cast<qint64>(record.value(QStringLiteral("size")).toDouble(-1))) {
            if (error && error->isEmpty()) *error = QStringLiteral("Backup file missing or changed: %1").arg(relative);
            return false;
        }
    }
    if (!hasDatabase) { if (error) *error = QStringLiteral("Backup has no project database."); return false; }
    ProjectInfo info;
    if (!ProjectStore::read(QDir(archiveDirectory).filePath(QStringLiteral("project.sqlite")), &info, error) ||
        info.id != object.value(QStringLiteral("project_id")).toString()) return false;
    QString integrity;
    if (!ProjectStore::integrityCheck(QDir(archiveDirectory).filePath(QStringLiteral("project.sqlite")), &integrity, error)) {
        if (error && error->isEmpty()) *error = integrity;
        return false;
    }
    for (const auto &entry : files) {
        const auto relative = entry.toObject().value(QStringLiteral("path")).toString();
        if (!copyChecked(QDir(archiveDirectory).filePath(relative), QDir(emptyDestination).filePath(relative), error)) return false;
    }
    return true;
}
