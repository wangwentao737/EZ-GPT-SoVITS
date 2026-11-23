#include "workspacepaths.h"
#include "engine.h"
#include "settings.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QReadWriteLock>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <algorithm>
#include <stdexcept>

namespace ez {
namespace {

const QString relativePrefix = QStringLiteral("data:");
QString dataRoot;
QStringList previousRoots;
QString migrationOutput;
QReadWriteLock externalLock;
QJsonObject engineLocations;
QJsonObject outputLocations;
QSet<QString> unresolvedContent;

[[noreturn]] void fail(const QString &message)
{
    throw std::runtime_error(message.toStdString());
}

void execute(QSqlQuery &query)
{
    if (!query.exec())
        fail(query.lastError().text());
}

QString normalized(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

QString below(const QString &path, const QString &parent)
{
    if (parent.isEmpty())
        return {};
    const auto prefix = parent.endsWith('/') ? parent : parent + '/';
    return path.startsWith(prefix, Qt::CaseInsensitive) ? path.mid(prefix.size()) : QString();
}

bool managed(const QString &relative)
{
    const auto category = relative.section('/', 0, 0).toLower();
    return category == "assets" || category == "projects" || category == "outputs" || category == "jobs";
}

QString relocated(const QString &path, const QJsonObject &locations)
{
    if (path.isEmpty() || path.startsWith(relativePrefix))
        return path;
    const auto clean = normalized(path);
    qsizetype longest = 0;
    QString target = path;
    for (auto item = locations.constBegin(); item != locations.constEnd(); ++item) {
        const auto &old = item.key();
        const auto relative = below(clean, old);
        if (old.size() > longest && (clean.compare(old, Qt::CaseInsensitive) == 0 || !relative.isEmpty())) {
            target = relative.isEmpty() ? item.value().toString() : QDir(item.value().toString()).filePath(relative);
            longest = old.size();
        }
    }
    return target;
}

QJsonObject updatedLocations(QJsonObject locations, const QString &previous, const QString &selected)
{
    const QJsonObject update{{previous.toCaseFolded(), selected}};
    for (auto item = locations.begin(); item != locations.end(); ++item)
        item.value() = relocated(item.value().toString(), update);
    locations[previous.toCaseFolded()] = selected;
    return locations;
}

QString mapPath(const QString &path, bool store, bool defaultOutput = true)
{
    if (path.isEmpty())
        return path;
    if (path.startsWith(relativePrefix)) {
        const auto relative = path.mid(relativePrefix.size());
        if (!managed(relative) || relative != normalized(relative) || relative.contains('\\'))
            fail(QStringLiteral("库内文件引用无效：%1").arg(path));
        return store ? path : checkedPath(QDir(dataRoot).filePath(relative));
    }
    const auto clean = normalized(path);
    if (!defaultOutput && (clean.compare(migrationOutput, Qt::CaseInsensitive) == 0
            || !below(clean, migrationOutput).isEmpty()))
        return path;
    auto relative = below(clean, dataRoot);
    if (!managed(relative)) {
        for (const auto &previous : previousRoots) {
            relative = below(clean, previous);
            if (managed(relative))
                break;
        }
    }
    if (!managed(relative))
        return path;
    return store ? relativePrefix + relative : checkedPath(QDir(dataRoot).filePath(relative));
}

bool customOutput(const QJsonObject &object)
{
    if (object.contains("external_output"))
        return object.value("external_output").toBool();
    const auto path = normalized(object.value("output_root").toString());
    if (path.startsWith(relativePrefix) || !QDir::isAbsolutePath(path))
        return false;
    const auto defaultAt = [&path](const QString &root) {
        return below(path, root).compare("outputs", Qt::CaseInsensitive) == 0;
    };
    return !defaultAt(dataRoot) && std::none_of(previousRoots.cbegin(), previousRoots.cend(), defaultAt);
}

// Only these fields contain library paths. Engine paths and historical input
// versions keep their original locations and file identities.
template<typename Transform>
QJsonObject transformPaths(QJsonObject object, const Transform &transform)
{
    for (const auto *name : {"directory", "project", "raw", "raw_pending", "seed_clips", "annotation", "source", "destination",
                              "gpt", "sovits", "reference", "weight", "audio", "output_root"}) {
        const auto value = object.value(name);
        if (value.isString())
            object[name] = transform(value.toString(), QString::fromLatin1(name));
    }
    const auto clips = object.value("clips");
    if (clips.isString()) {
        object["clips"] = transform(clips.toString(), QStringLiteral("clips"));
    } else if (clips.isArray()) {
        QJsonArray paths;
        for (const auto &path : clips.toArray())
            paths.append(path.isString() ? QJsonValue(transform(path.toString(), QStringLiteral("clips"))) : path);
        object["clips"] = paths;
    }
    return object;
}

struct Record {
    QString table;
    QString column;
    QString id;
    QString state;
    QString operation;
    QJsonObject value;
};

QList<Record> records()
{
    QList<Record> rows;
    const auto tables = QSqlDatabase::database().tables();
    for (const auto &[table, column] : {std::pair{"projects", "state"}, {"voices", "settings"},
                                        {"jobs", "request"}, {"jobs", "result"}}) {
        if (!tables.contains(table))
            continue;
        QSqlQuery query;
        query.prepare(QString("SELECT id,%1,%2 FROM %3 WHERE %1 IS NOT NULL").arg(column,
            QString(table) == "jobs" ? "state,operation" : "'',''", table));
        execute(query);
        while (query.next()) {
            const auto doc = QJsonDocument::fromJson(query.value(1).toByteArray());
            if (doc.isObject())
                rows.append({table, column, query.value(0).toString(), query.value(2).toString(),
                    query.value(3).toString(), doc.object()});
        }
    }
    return rows;
}

void writeRecord(const Record &row, const QJsonObject &object)
{
    QSqlQuery query;
    query.prepare(QString("UPDATE %1 SET %2=? WHERE id=?").arg(row.table, row.column));
    query.addBindValue(QJsonDocument(object).toJson(QJsonDocument::Compact));
    query.addBindValue(row.id);
    execute(query);
}

QString digest(const QString &path)
{
    QFile file(checkedPath(path));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? QString::fromLatin1(hash.result().toHex()) : QString();
}

QStringList candidateRoots(const QList<Record> &rows)
{
    QSet<QString> candidates;
    const QRegularExpression projectPattern(R"(^([a-z]:/.*)/projects/([a-f0-9]{32})$)",
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression assetPattern(R"(^([a-z]:/.*)/(assets/(?:models|audio)/.+)$)",
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpression wavePattern(R"(^([a-z]:/.*)/(outputs/(?:[^/]+/)?[^/]+\.wav)$)",
        QRegularExpression::CaseInsensitiveOption);
    for (const auto &row : rows) {
        const auto project = projectPattern.match(normalized(row.value.value("directory").toString()));
        if (project.hasMatch() && project.captured(2) == row.id
            && QFileInfo(QDir(dataRoot).filePath("projects/" + row.id)).isDir())
            candidates.insert(project.captured(1));
        for (const auto *key : {"gpt", "sovits", "reference", "audio"}) {
            const auto path = normalized(row.value.value(key).toString());
            const bool output = QString(key) == "audio";
            const auto match = (output ? wavePattern : assetPattern).match(path);
            if (!match.hasMatch() || (!output && QFileInfo(path).baseName().size() != 64)
                || (output && QFileInfo(path).baseName() != row.id)
                || (output && QFileInfo(match.captured(1)).fileName().compare("data", Qt::CaseInsensitive) != 0))
                continue;
            if (candidates.contains(match.captured(1)))
                continue;
            const auto target = QDir(dataRoot).filePath(match.captured(2));
            const auto expected = output ? row.value.value("sha256").toString() : QFileInfo(path).baseName();
            try {
                if (QFileInfo(target).isFile() && !expected.isEmpty() && digest(target) == expected)
                    candidates.insert(match.captured(1));
            } catch (const std::exception &) {
                // A path that fails validation is not evidence of a previous workspace.
            }
        }
    }
    QStringList result;
    for (const auto &candidate : candidates) {
        if (!result.contains(candidate, Qt::CaseInsensitive))
            result.append(candidate);
    }
    return result;
}

struct AnnotationCopy {
    QString path;
    QByteArray content;
};

AnnotationCopy migrateAnnotation(const QJsonObject &original, const QJsonObject &resolved)
{
    const auto old = original.value("annotation").toString();
    const auto annotation = resolved.value("annotation").toString();
    const auto clips = resolved.value("clips").toString();
    if (old.isEmpty() || clips.isEmpty() || annotation.isEmpty() || previousRoots.isEmpty())
        return {};
    if (!managed(below(annotation, dataRoot)) || !managed(below(clips, dataRoot)))
        return {};
    QFile file(checkedPath(annotation));
    if (!file.open(QIODevice::ReadOnly))
        fail(QStringLiteral("找不到标注 %1").arg(annotation));
    const auto content = file.readAll();
    auto lines = content.split('\n');
    bool changed = false;
    for (auto &line : lines) {
        if (line.trimmed().isEmpty())
            continue;
        const auto separator = line.indexOf('|');
        if (separator < 0)
            fail(QStringLiteral("标注行缺少路径分隔符 %1").arg(annotation));
        const bool bom = line.startsWith("\xef\xbb\xbf");
        const auto source = QString::fromUtf8(line.mid(bom ? 3 : 0, separator - (bom ? 3 : 0)));
        const auto mapped = resolvedWorkspacePath(source);
        if (mapped == source)
            continue;
        if (below(mapped, clips).isEmpty() || !QFileInfo(checkedPath(mapped)).isFile())
            fail(QStringLiteral("标注片段需要重新绑定 %1").arg(source));
        line = (bom ? QByteArray("\xef\xbb\xbf") : QByteArray()) + mapped.toUtf8() + line.mid(separator);
        changed = true;
    }
    if (!changed)
        return {};
    const auto replacement = lines.join('\n');
    const auto hash = QCryptographicHash::hash(replacement, QCryptographicHash::Sha256).toHex();
    return {checkedPath(QFileInfo(annotation).absolutePath() + "/relocated-" + QString::fromLatin1(hash) + ".list"),
        replacement};
}

void invalidateTraining(QJsonObject &project)
{
    QJsonObject prior;
    const bool canPreview = project.contains("gpt") && project.contains("sovits");
    for (const auto *key : {"dataset", "sampleCount", "gpt", "sovits", "engine", "experiment"}) {
        if (project.contains(key))
            prior[key] = canPreview && (QString(key) == "gpt" || QString(key) == "sovits")
                ? project.value(key) : project.take(key);
    }
    if (!prior.isEmpty())
        project["prior_training"] = prior;
    project["training_recovery"] = QStringLiteral("数据目录已搬迁，标注路径发生变化。请核对标注并重新准备数据，然后训练。已入库声音可继续用于合成。");
}

}

QString resolvedWorkspacePath(const QString &path)
{
    return mapPath(path, false);
}

QJsonObject storedWorkspacePaths(QJsonObject object)
{
    const bool external = object.value("external_output").toBool();
    return transformPaths(std::move(object), [external](const QString &path, const QString &field) {
        if (external && (field == "audio" || field == "output_root"))
            return path;
        return mapPath(path, true, field != "audio" && field != "output_root");
    });
}

QJsonObject resolvedWorkspacePaths(QJsonObject object)
{
    {
        QReadLocker guard(&externalLock);
        for (const auto *field : {"engine", "dataset", "checkpoints"}) {
            if (object.value(field).isString())
                object[field] = relocated(object.value(field).toString(), engineLocations);
        }
    }
    const bool external = customOutput(object);
    if (external)
        object["external_output"] = true;
    return transformPaths(std::move(object), [external](const QString &path, const QString &field) {
        const bool output = field == "audio" || field == "output_root";
        if (output) {
            const auto rebound = resolvedOutputDirectory(path);
            if (external || rebound != path)
                return rebound;
        }
        return mapPath(path, false, !output);
    });
}

static QJsonObject validateWorkspaceRecord(QJsonObject object)
{
    object = resolvedWorkspacePaths(std::move(object));
    for (const auto *field : {"gpt", "sovits", "reference", "weight", "audio"}) {
        if (unresolvedContent.contains(below(object.value(field).toString(), dataRoot).toCaseFolded()))
            object.remove(field);
    }
    return object;
}

QJsonObject readWorkspaceRecord(const QByteArray &json)
{
    return validateWorkspaceRecord(QJsonDocument::fromJson(json).object());
}

QJsonObject readWorkspaceResult(const QByteArray &json, const QJsonObject &request)
{
    auto result = QJsonDocument::fromJson(json).object();
    if (customOutput(request))
        result["external_output"] = true;
    return validateWorkspaceRecord(std::move(result));
}

bool openWorkspacePaths(const QString &root, const std::function<bool(const QString &)> &confirm)
{
    dataRoot = root;
    auto database = QSqlDatabase::database();
    QSqlQuery query(database);
    query.prepare("PRAGMA user_version");
    execute(query);
    const int version = query.next() ? query.value(0).toInt() : 0;
    query.finish();
    if (version > 1)
        fail(QStringLiteral("此素材库由更新版本创建，请使用该版本打开。"));
    const auto lastRoot = workspaceSetting("dataRoot");
    for (const auto &value : QJsonDocument::fromJson(workspaceSetting("previousDataRoots").toUtf8()).array())
        previousRoots.append(value.toString());
    migrationOutput = workspaceSetting("legacyOutputRoot");
    engineLocations = QJsonDocument::fromJson(workspaceSetting("engineLocations").toUtf8()).object();
    outputLocations = QJsonDocument::fromJson(workspaceSetting("outputLocations").toUtf8()).object();
    auto conflicts = QJsonDocument::fromJson(workspaceSetting("migrationConflicts").toUtf8()).object();
    for (auto item = conflicts.constBegin(); item != conflicts.constEnd(); ++item) {
        try {
            if (digest(QDir(dataRoot).filePath(item.key())) != item.value().toString())
                unresolvedContent.insert(item.key());
        } catch (const std::exception &) {
            unresolvedContent.insert(item.key());
        }
    }
    if (version == 1 && lastRoot.compare(root, Qt::CaseInsensitive) == 0)
        return true;

    const auto rows = records();
    QStringList issues;
    if (!lastRoot.isEmpty()) {
        if (!previousRoots.contains(lastRoot, Qt::CaseInsensitive))
            previousRoots.prepend(lastRoot);
    } else if (!rows.isEmpty()) {
        migrationOutput = workspaceSetting("outputRoot");
        const auto inferred = candidateRoots(rows);
        if (inferred.size() == 1 && inferred.first().compare(root, Qt::CaseInsensitive) != 0)
            previousRoots.append(inferred.first());
        if (inferred.size() > 1)
            issues.append(QStringLiteral("检测到多处旧数据根，需核对引用后重新绑定：%1").arg(inferred.join(" · ")));
    }
    previousRoots.removeAll(root);
    QMap<QString, AnnotationCopy> copies;
    QList<QPair<Record, QJsonObject>> changes;
    QSet<QString> interrupted;
    QSet<QString> missing;
    QSet<QString> external;
    QMap<QString, QString> validated;
    qsizetype mapped = 0;
    for (const auto &row : rows) {
        const auto original = row.value;
        const bool editable = row.table != "jobs" || (row.column == "request" && row.state == "queued");
        try {
            auto resolved = resolvedWorkspacePaths(original);
            transformPaths(original, [&](const QString &path, const QString &field) {
                const auto current = resolved.value(field);
                const auto target = current.isString() ? current.toString() : resolvedWorkspacePath(path);
                if (target != path && (!path.startsWith(relativePrefix) || !previousRoots.isEmpty()))
                    ++mapped;
                if (!path.isEmpty() && !QFileInfo(target).exists() && field != "destination" && field != "output_root")
                    missing.insert(path);
                return path;
            });
            for (const auto *field : {"engine", "dataset", "checkpoints", "output_root"}) {
                const auto path = original.value(field).toString();
                if (!path.isEmpty() && !path.startsWith(relativePrefix) && resolved.value(field) == path
                    && QDir::isAbsolutePath(path))
                    external.insert(path);
            }
            if (!previousRoots.isEmpty()) {
                for (const auto *field : {"gpt", "sovits", "reference", "audio"}) {
                    const auto target = resolved.value(field).toString();
                    const auto relative = below(target, dataRoot).toCaseFolded();
                    const auto name = QFileInfo(target).baseName().toLower();
                    const auto expected = QString(field) == "audio" ? original.value("sha256").toString()
                        : relative.startsWith("assets/") ? name : QString();
                    static const QRegularExpression sha256("^[a-f0-9]{64}$");
                    if (relative.isEmpty() || !sha256.match(expected).hasMatch() || !QFileInfo(target).isFile())
                        continue;
                    if (!validated.contains(relative))
                        validated.insert(relative, digest(target));
                    if (validated.value(relative) != expected) {
                        conflicts[relative] = expected;
                        unresolvedContent.insert(relative);
                        issues.append(QStringLiteral("文件摘要不符，请恢复原文件后重启 %1").arg(target));
                    }
                }
            }
            if (!editable)
                continue;
            if (!previousRoots.isEmpty() && (row.table == "projects" || row.operation == "prepare"
                    || row.operation.startsWith("train_"))) {
                const auto copy = migrateAnnotation(original, resolved);
                if (!copy.path.isEmpty()) {
                    copies.insert(copy.path, copy);
                    resolved["annotation"] = copy.path;
                    if (row.table == "projects")
                        invalidateTraining(resolved);
                    else if (row.operation.startsWith("train_"))
                        interrupted.insert(row.id);
                }
                if (row.operation == "prepare" && !copy.path.isEmpty())
                    resolved["experiment"] = "ez_" + QUuid::createUuid().toString(QUuid::Id128);
            }
            const auto stored = storedWorkspacePaths(resolved);
            if (stored != original)
                changes.append({row, stored});
        } catch (const std::exception &error) {
            issues.append(QString::fromUtf8(error.what()));
            if (row.state == "queued" && row.column == "request"
                && (row.operation.startsWith("train_") || row.operation == "prepare"))
                interrupted.insert(row.id);
        }
    }
    query.prepare("SELECT launcher FROM engines");
    execute(query);
    while (query.next())
        external.insert(query.value(0).toString());
    query.finish();
    for (const auto &path : external) {
        if (!QFileInfo(path).exists())
            missing.insert(path);
    }
    const bool relocation = !previousRoots.isEmpty() || !issues.isEmpty() || (!rows.isEmpty() && !missing.isEmpty());
    if (relocation && confirm) {
        QStringList lines{QStringLiteral("当前数据目录 %1").arg(root),
            QStringLiteral("可映射引用 %1 项，将另存标注 %2 份，需要重新准备数据的排队任务 %3 项。")
                .arg(mapped).arg(copies.size()).arg(interrupted.size())};
        if (!previousRoots.isEmpty())
            lines.append(QStringLiteral("原数据目录\n%1").arg(previousRoots.join('\n')));
        if (!external.isEmpty())
            lines.append(QStringLiteral("外部位置\n%1").arg(QStringList(external.values()).join('\n')));
        if (!missing.isEmpty())
            lines.append(QStringLiteral("待重新定位的文件\n%1").arg(QStringList(missing.values()).join('\n')));
        lines.append(issues);
        if (!confirm(lines.join("\n\n")))
            return false;
    }
    if (!rows.isEmpty()) {
        query.prepare("VACUUM INTO ?");
        query.addBindValue(checkedPath(root + "/library-before-migration-"
            + QUuid::createUuid().toString(QUuid::Id128) + ".sqlite"));
        execute(query);
    }
    if (!database.transaction())
        fail(database.lastError().text());
    try {
        for (const auto &copy : copies) {
            QSaveFile file(copy.path);
            if (!file.open(QIODevice::WriteOnly) || file.write(copy.content) != copy.content.size() || !file.commit())
                fail(QStringLiteral("无法写入迁移标注 %1").arg(copy.path));
            QFile check(copy.path);
            if (!check.open(QIODevice::ReadOnly) || check.readAll() != copy.content)
                fail(QStringLiteral("无法核对迁移标注 %1").arg(copy.path));
        }
        for (const auto &[row, value] : changes)
            writeRecord(row, value);
        for (const auto &id : interrupted) {
            query.prepare("UPDATE jobs SET state='interrupted',message=? WHERE id=? AND state='queued'");
            query.addBindValue(QStringLiteral("搬迁后训练输入路径已变化。请核对标注、重新准备数据，再从训练项目开始。"));
            query.addBindValue(id);
            execute(query);
        }
        setWorkspaceSetting("dataRoot", root);
        setWorkspaceSetting("legacyOutputRoot", migrationOutput);
        setWorkspaceSetting("migrationConflicts", QString::fromUtf8(QJsonDocument(conflicts).toJson(QJsonDocument::Compact)));
        setWorkspaceSetting("previousDataRoots", QString::fromUtf8(QJsonDocument(
            QJsonArray::fromStringList(previousRoots)).toJson(QJsonDocument::Compact)));
        query.prepare("PRAGMA user_version=1");
        execute(query);
        if (!database.commit())
            fail(database.lastError().text());
    } catch (...) {
        database.rollback();
        throw;
    }
    return true;
}

QString resolvedEngineLauncher(const QString &launcher)
{
    QReadLocker guard(&externalLock);
    return relocated(launcher, engineLocations);
}

QString resolvedOutputDirectory(const QString &directory)
{
    QReadLocker guard(&externalLock);
    return relocated(directory, outputLocations);
}

QStringList workspaceOutputDirectories()
{
    QStringList paths;
    const auto configured = resolvedOutputDirectory(workspaceSetting("outputRoot"));
    if (!configured.isEmpty())
        paths.append(configured);
    QSqlQuery query;
    query.prepare("SELECT DISTINCT json_extract(CAST(request AS TEXT),'$.output_root'),"
        "json_extract(CAST(request AS TEXT),'$.external_output') FROM jobs "
        "WHERE operation='synthesize' AND json_valid(CAST(request AS TEXT))");
    execute(query);
    while (query.next()) {
        const auto path = query.value(0).toString();
        QJsonObject request{{"output_root", path}};
        if (!query.value(1).isNull())
            request["external_output"] = query.value(1).toBool();
        if (!customOutput(request))
            continue;
        const auto current = resolvedOutputDirectory(path);
        if (!paths.contains(current, Qt::CaseInsensitive))
            paths.append(current);
    }
    return paths;
}

bool relocateWorkspaceEngine(const QString &previous, const QString &selected,
    const std::function<bool(const QString &)> &confirm)
{
    const auto target = engineRoot(selected);
    const auto source = QFileInfo(normalized(previous)).path();
    if (source.compare(target, Qt::CaseInsensitive) == 0)
        return false;
    QList<Record> projects;
    QStringList affected;
    for (const auto &row : records()) {
        if (row.table == "projects" && resolvedEngineLauncher(row.value.value("engine").toString())
                .compare(previous, Qt::CaseInsensitive) == 0) {
            affected.append(row.value.value("name").toString());
            projects.append(row);
        }
    }
    if (!confirm(QStringLiteral("将引擎定位到 %1。\n受影响项目 %2。\n已有训练产物可试听。继续训练前请用新引擎重新准备数据。")
            .arg(target, affected.isEmpty() ? QStringLiteral("无") : affected.join(QStringLiteral("、")))))
        return false;

    const auto launcher = target + "/go-webui.bat";
    const auto locations = updatedLocations(engineLocations, source, target);
    auto database = QSqlDatabase::database();
    if (!database.transaction())
        fail(database.lastError().text());
    try {
        QSqlQuery query;
        query.prepare("UPDATE engines SET launcher=?,name=? WHERE launcher=?");
        query.addBindValue(launcher);
        query.addBindValue(QFileInfo(target).fileName());
        query.addBindValue(previous);
        execute(query);
        query.prepare("UPDATE jobs SET launcher=? WHERE state='queued' AND operation NOT LIKE 'train_%' AND launcher=? COLLATE NOCASE");
        query.addBindValue(launcher);
        query.addBindValue(previous);
        execute(query);
        query.prepare("UPDATE jobs SET state='interrupted',message=? WHERE state='queued' AND operation LIKE 'train_%' AND launcher=? COLLATE NOCASE");
        query.addBindValue(QStringLiteral("引擎已重新定位，请用新引擎重新准备数据，再从训练项目开始。"));
        query.addBindValue(previous);
        execute(query);
        for (const auto &row : projects) {
            auto state = row.value;
            if (state.contains("dataset") && !(state.contains("gpt") && state.contains("sovits"))) {
                invalidateTraining(state);
                state["training_recovery"] = QStringLiteral("引擎位置已更改。请重新准备数据，然后开始训练。");
            }
            writeRecord(row, state);
        }
        setWorkspaceSetting("currentEngine", launcher);
        setWorkspaceSetting("engineLocations", QString::fromUtf8(QJsonDocument(locations).toJson(QJsonDocument::Compact)));
        if (!database.commit())
            fail(database.lastError().text());
    } catch (...) {
        database.rollback();
        throw;
    }
    QWriteLocker guard(&externalLock);
    engineLocations = locations;
    return true;
}

bool relocateWorkspaceOutput(const QString &previous, const QString &selected,
    const std::function<bool(const QString &)> &confirm)
{
    const auto target = checkedPath(selected);
    if (!QFileInfo(target).isDir() || !QFileInfo(target).isWritable())
        fail(QStringLiteral("请选择可写的成品目录。"));
    if (previous.compare(target, Qt::CaseInsensitive) == 0)
        return false;
    qsizetype jobs = 0;
    QSet<QString> voices;
    QSet<QString> projectIds;
    QMap<QString, QString> projectNames;
    const auto rows = records();
    for (const auto &row : rows) {
        if (row.table == "projects")
            projectNames.insert(row.id, row.value.value("name").toString());
        if (row.table != "jobs")
            continue;
        const auto path = resolvedOutputDirectory(row.value.value(row.column == "request" ? "output_root" : "audio").toString());
        if (normalized(path).compare(previous, Qt::CaseInsensitive) != 0 && below(normalized(path), previous).isEmpty())
            continue;
        if (row.column == "request") {
            ++jobs;
            const auto project = row.value.value("project_id").toString();
            if (!project.isEmpty())
                projectIds.insert(project);
            const auto voice = row.value.value("name").toString();
            if (!voice.isEmpty())
                voices.insert(voice);
        } else {
            const auto moved = QDir(target).filePath(below(normalized(path), previous));
            const auto expected = row.value.value("sha256").toString();
            if (QFileInfo(moved).isFile() && !expected.isEmpty() && digest(moved) != expected)
                fail(QStringLiteral("新目录文件与任务摘要不符，请检查后再定位。\n%1").arg(moved));
        }
    }
    QStringList projects;
    for (const auto &id : projectIds) {
        if (projectNames.contains(id))
            projects.append(projectNames.value(id));
    }
    if (!confirm(QStringLiteral("将 %1 定位到 %2。\n影响 %3 条任务及其历史音频。\n声音 %4。\n项目 %5。")
            .arg(previous, target).arg(jobs).arg(voices.isEmpty() ? QStringLiteral("无记录")
                : QStringList(voices.values()).join(QStringLiteral("、")))
            .arg(projects.isEmpty() ? QStringLiteral("无记录") : projects.join(QStringLiteral("、")))))
        return false;
    const auto locations = updatedLocations(outputLocations, previous, target);
    setWorkspaceSetting("outputLocations", QString::fromUtf8(QJsonDocument(locations).toJson(QJsonDocument::Compact)));
    QWriteLocker guard(&externalLock);
    outputLocations = locations;
    return true;
}

}
