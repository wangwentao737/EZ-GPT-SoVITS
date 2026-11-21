#include "jobs.h"
#include "engine.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <stdexcept>
#include <utility>

namespace ez {
namespace {

void fail(const QString &message)
{
    throw std::runtime_error(message.toStdString());
}

void execute(QSqlQuery &query)
{
    if (!query.exec())
        fail(query.lastError().text());
}

void save(const QString &path, const QByteArray &content)
{
    QSaveFile file(checkedPath(path));
    if (!file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit())
        fail(QStringLiteral("无法保存 %1").arg(path));
}

QByteArray json(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonObject readResult(const QString &path)
{
    QFile file(checkedPath(path));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

}

void startJobProcess(QProcess &worker, const QString &engine, const QString &root,
                     const QString &job, QJsonObject request)
{
    const auto directory = checkedPath(job);
    if (!QDir().mkpath(directory))
        fail(QStringLiteral("无法创建任务目录"));
    request.insert("engine", engine);
    request.insert("data", root);
    request.insert("job", directory);
    save(directory + "/request.json", json(request));
    QFile embedded(":/engine/bridge.py");
    if (!embedded.open(QIODevice::ReadOnly))
        fail(QStringLiteral("无法读取引擎适配程序"));
    save(directory + "/bridge.py", embedded.readAll());
    worker.setWorkingDirectory(engine);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("PATH", QDir::toNativeSeparators(engine + "/runtime") + ';' + environment.value("PATH"));
    worker.setProcessEnvironment(environment);
    worker.setStandardOutputFile(checkedPath(directory + "/result.log"));
    worker.setStandardErrorFile(checkedPath(directory + "/error.log"));
    worker.setProgram(checkedPath(engine + "/runtime/python.exe"));
    worker.setArguments({"-I", "-X", "utf8", "-B", "-u", directory + "/bridge.py", directory + "/request.json"});
    worker.start();
}

TaskScheduler::TaskScheduler(QString dataRoot, EngineManager &engine, QObject *parent)
    : QObject(parent), root_(std::move(dataRoot)), engine_(engine)
{
    QSqlQuery query;
    query.prepare("CREATE TABLE IF NOT EXISTS jobs ("
                  "id TEXT PRIMARY KEY, created TEXT NOT NULL, launcher TEXT NOT NULL,"
                  "operation TEXT NOT NULL, state TEXT NOT NULL, request BLOB NOT NULL,"
                  "message TEXT NOT NULL DEFAULT '', result BLOB)");
    execute(query);
    query.prepare("SELECT id FROM jobs WHERE state='running' AND operation='synthesize'");
    execute(query);
    QStringList unfinished;
    while (query.next())
        unfinished.append(query.value(0).toString());
    query.finish();
    for (const auto &id : unfinished) {
        try {
            const auto completion = readResult(root_ + "/jobs/" + id + "/completion.json");
            const auto audio = completion.value("audio").toString();
            if (!audio.startsWith(root_ + "/outputs/") || QFileInfo(audio).fileName() != id + ".wav")
                continue;
            QFile file(checkedPath(audio));
            if (!file.open(QIODevice::ReadOnly))
                continue;
            const auto header = file.read(12);
            if (header.startsWith("RIFF") && header.mid(8) == "WAVE" && file.size() > 44)
                update(id, "succeeded", {}, completion);
        } catch (const std::exception &) {
            // An unavailable artifact stays in the interrupted task list.
        }
    }
    query.prepare("UPDATE jobs SET state='interrupted' WHERE state='running'");
    execute(query);
    connect(&engine_, &EngineManager::readinessChanged, this, [this](bool ready) {
        if (ready)
            next();
        else if (running() && !cancelling_)
            worker_.kill();
    });
    connect(&worker_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        finish(code == 0 && status == QProcess::NormalExit, {});
    });
    connect(&worker_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(false, worker_.errorString());
    });
}

QString TaskScheduler::enqueue(const QString &launcher, const QJsonObject &request)
{
    const auto root = engineRoot(launcher);
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlQuery query;
    query.prepare("INSERT INTO jobs(id, created, launcher, operation, state, request)"
                  "VALUES (?, ?, ?, ?, 'queued', ?)");
    query.addBindValue(id);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(root + "/go-webui.bat");
    query.addBindValue(request.value("operation").toString());
    query.addBindValue(json(request));
    execute(query);
    emit changed();
    if (!engine_.active()) {
        try {
            engine_.start(launcher, root_ + "/engine-start.log");
        } catch (const std::exception &error) {
            update(id, "failed", QString::fromUtf8(error.what()));
            throw;
        }
    }
    next();
    return id;
}

void TaskScheduler::next()
{
    if (stopping_ || !engine_.ready() || running())
        return;
    try {
        QSqlQuery query;
        query.prepare("SELECT id, request FROM jobs WHERE state='queued'"
                      " AND launcher=? COLLATE NOCASE ORDER BY created, rowid LIMIT 1");
        query.addBindValue(engine_.root() + "/go-webui.bat");
        execute(query);
        if (!query.next())
            return;
        active_ = query.value(0).toString();
        activeRequest_ = QJsonDocument::fromJson(query.value(1).toByteArray()).object();
        query.finish();
        jobPath_ = checkedPath(root_ + "/jobs/" + active_);
        update(active_, "running");
        startJobProcess(worker_, engine_.root(), root_, jobPath_, activeRequest_);
    } catch (const std::exception &error) {
        finish(false, QString::fromUtf8(error.what()));
    }
}

void TaskScheduler::finish(bool processSucceeded, QString message)
{
    if (!running())
        return;
    const auto id = std::exchange(active_, {});
    const auto job = std::exchange(jobPath_, {});
    const auto request = std::exchange(activeRequest_, {});
    const bool cancelled = std::exchange(cancelling_, false);
    QJsonObject result;
    try {
        if (cancelled) {
            update(id, "cancelled");
            return;
        }
        if (processSucceeded) {
            QFile log(checkedPath(job + "/result.log"));
            if (log.open(QIODevice::ReadOnly)) {
                while (!log.atEnd()) {
                    const auto line = log.readLine();
                    if (line.startsWith("EZ_RESULT "))
                        result = QJsonDocument::fromJson(line.mid(10)).object();
                }
            }
            if (result.isEmpty())
                fail(QStringLiteral("引擎未提交有效的任务产物"));
            if (request.value("operation") == "synthesize") {
                const auto source = checkedPath(job + "/verified.wav");
                const auto voice = QUuid(request.value("voice_id").toString()).toString(QUuid::WithoutBraces);
                const auto directory = checkedPath(root_ + "/outputs/" + voice + "/"
                    + QDate::currentDate().toString(Qt::ISODate));
                const auto output = checkedPath(directory + "/" + id + ".wav");
                result.insert("audio", output);
                save(job + "/completion.json", json(result));
                if (!QDir().mkpath(directory) || !QFile::rename(source, output))
                    fail(QStringLiteral("无法归档音频，请检查磁盘空间和输出目录权限"));
            }
            update(id, "succeeded", {}, result);
            emit completed(id, result);
        } else {
            if (message.isEmpty()) {
                QFile log(checkedPath(job + "/error.log"));
                if (log.open(QIODevice::ReadOnly)) {
                    log.seek(qMax<qint64>(0, log.size() - 4096));
                    const auto detail = QString::fromUtf8(log.readAll()).trimmed();
                    message = detail.section('\n', -1);
                }
            }
            if (message.isEmpty())
                message = QStringLiteral("任务中断，详情见任务日志");
            update(id, "failed", message);
            emit failed(message);
        }
    } catch (const std::exception &error) {
        const auto detail = QString::fromUtf8(error.what());
        try {
            update(id, "failed", detail);
        } catch (const std::exception &) {
            // The next startup marks a still-running record as interrupted.
        }
        emit failed(detail);
    }
    QTimer::singleShot(0, this, &TaskScheduler::next);
}

void TaskScheduler::update(const QString &id, const QString &state, const QString &message,
                           const QJsonObject &result)
{
    QSqlQuery query;
    query.prepare("UPDATE jobs SET state=?, message=?, result=? WHERE id=?");
    query.addBindValue(state);
    query.addBindValue(message.isNull() ? QStringLiteral("") : message);
    query.addBindValue(json(result));
    query.addBindValue(id);
    execute(query);
    emit changed();
}

void TaskScheduler::cancel(const QString &id)
{
    if (active_ == id) {
        cancelling_ = true;
        engine_.stop();
        worker_.kill();
        return;
    }
    QSqlQuery query;
    query.prepare("UPDATE jobs SET state='cancelled' WHERE id=? AND state='queued'");
    query.addBindValue(id);
    execute(query);
    emit changed();
}

void TaskScheduler::stop()
{
    stopping_ = true;
    if (running()) {
        cancel(active_);
        worker_.waitForFinished(3000);
    }
}

bool TaskScheduler::running() const
{
    return !active_.isEmpty();
}

}
