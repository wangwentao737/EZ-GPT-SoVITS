#include "jobs.h"
#include "engine.h"
#include "resources.h"
#include "settings.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QUuid>
#include <QtEndian>
#include <stdexcept>
#include <utility>

namespace ez {
namespace {

constexpr int cancelledExitCode = 10;
constexpr int cancelGraceMs = 5000;

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

bool isArchivedWave(const QString &path, const QJsonObject &receipt)
{
    QFile file(checkedPath(path));
    if (!file.open(QIODevice::ReadOnly) || file.size() <= 44)
        return false;
    const auto header = file.read(12);
    if (header.size() != 12 || !header.startsWith("RIFF") || header.mid(8) != "WAVE"
        || qFromLittleEndian<quint32>(header.constData() + 4) + qint64(8) != file.size()) {
        return false;
    }
    if (!receipt.contains("sha256") && !receipt.contains("bytes"))
        return true; // Receipts written before audio checksums were recorded.
    const auto digest = receipt.value("sha256").toString().toLatin1();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return receipt.value("bytes").toInteger() == file.size() && digest.size() == 64
        && file.seek(0) && hash.addData(&file) && hash.result().toHex() == digest;
}

QString outputRootForRequest(const QString &dataRoot, const QJsonObject &request)
{
    const auto saved = request.value("output_root").toString();
    return checkedWorkspaceRoot(saved.isEmpty() ? dataRoot + "/outputs" : saved);
}

void checkOutputDirectory(const QString &path)
{
    QFileInfo directory(path);
    while (!directory.exists()) {
        const auto parent = directory.absolutePath();
        if (parent == directory.absoluteFilePath())
            fail(QStringLiteral("找不到生成目录所在的磁盘，请在「设置」更改生成目录"));
        directory.setFile(parent);
    }
    const QStorageInfo disk(directory.absoluteFilePath());
    if (!directory.isDir() || !directory.isWritable() || disk.isReadOnly())
        fail(QStringLiteral("生成目录不可写，请在「设置」选择可写目录。\n%1").arg(path));
    if (disk.isReady() && disk.bytesAvailable() == 0)
        fail(QStringLiteral("生成目录所在磁盘空间已满，请释放空间或更改生成目录。\n%1").arg(path));
}

QString recoveryHint(const QString &message, const QString &logs, const QString &operation)
{
    const auto text = (message + '\n' + logs).toLower();
    if ((text.contains("out of memory") && (text.contains("cuda") || text.contains("gpu")))
        || text.contains(QStringLiteral("显存不足"))) {
        const auto action = operation.startsWith("train_") ? QStringLiteral("请降低训练批量后重试。")
                                                          : QStringLiteral("请释放显存后重试。");
        return QStringLiteral("显存不足。%1\n%2").arg(action, message);
    }
    if (text.contains("no space left on device") || text.contains("disk full") || text.contains("winerror 112"))
        return QStringLiteral("写入空间不足。请释放磁盘空间，或在「设置」更改生成目录后重新提交。\n%1").arg(message);
    return message;
}

void publishWave(const QString &source, const QString &target)
{
    QFile input(source);
    QSaveFile output(target);
    if (QFileInfo::exists(target) || !input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
        fail(QStringLiteral("无法归档音频，请检查生成目录权限和可用空间"));
    while (!input.atEnd()) {
        const auto block = input.read(1024 * 1024);
        if (input.error() != QFileDevice::NoError || output.write(block) != block.size())
            fail(QStringLiteral("音频写入中断，请检查生成目录权限和可用空间"));
    }
    if (!output.commit())
        fail(QStringLiteral("无法保存成品音频：%1").arg(output.errorString()));
    input.close();
    QFile::remove(source);
}

}

void startJobProcess(QProcess &worker, const QString &engine, const QString &root,
                     const QString &job, QJsonObject request, quint32 webuiPid)
{
    const auto directory = checkedPath(job);
    if (!QDir().mkpath(directory))
        fail(QStringLiteral("无法创建任务目录"));
    request.insert("engine", engine);
    request.insert("data", root);
    request.insert("job", directory);
    request.insert("webui_pid", static_cast<qint64>(webuiPid));
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

TaskScheduler::TaskScheduler(QString dataRoot, QString defaultOutputRoot, EngineManager &engine, QObject *parent)
    : QObject(parent), root_(std::move(dataRoot)), defaultOutputRoot_(std::move(defaultOutputRoot)), engine_(engine)
{
    QSqlQuery query;
    query.prepare("CREATE TABLE IF NOT EXISTS jobs ("
                  "id TEXT PRIMARY KEY, created TEXT NOT NULL, launcher TEXT NOT NULL,"
                  "operation TEXT NOT NULL, state TEXT NOT NULL, request BLOB NOT NULL,"
                  "message TEXT NOT NULL DEFAULT '', result BLOB)");
    execute(query);
    query.prepare("SELECT id,request FROM jobs WHERE state='running' AND operation='synthesize'");
    execute(query);
    QList<QPair<QString, QJsonObject>> unfinished;
    while (query.next())
        unfinished.append({query.value(0).toString(), QJsonDocument::fromJson(query.value(1).toByteArray()).object()});
    query.finish();
    for (const auto &[id, request] : unfinished) {
        try {
            const auto completion = readResult(root_ + "/jobs/" + id + "/completion.json");
            const auto audio = completion.value("audio").toString();
            const auto output = outputRootForRequest(root_, request);
            if (!audio.startsWith(output.endsWith('/') ? output : output + '/', Qt::CaseInsensitive)
                || QDir::cleanPath(audio) != audio
                || QFileInfo(audio).fileName() != id + ".wav")
                continue;
            if (isArchivedWave(audio, completion))
                update(id, "succeeded", {}, completion);
        } catch (const std::exception &) {
            // An unavailable artifact stays in the interrupted task list.
        }
    }
    query.prepare("UPDATE jobs SET state='interrupted' WHERE state='running'");
    execute(query);
    cancelTimeout_.setSingleShot(true);
    connect(&cancelTimeout_, &QTimer::timeout, this, &TaskScheduler::forceCancel);
    connect(&engine_, &EngineManager::readinessChanged, this, [this](bool ready) {
        if (ready)
            next();
        else if (running() && !cancelling_)
            worker_.kill();
    });
    connect(&worker_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (running() && cancelling_ && (status != QProcess::NormalExit || (code != 0 && code != cancelledExitCode)))
            forceCancel();
        finish(code == 0 && status == QProcess::NormalExit, {});
    });
    connect(&worker_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            if (cancelling_)
                forceCancel();
            finish(false, worker_.errorString());
        }
    });
}

QString TaskScheduler::enqueue(const QString &launcher, const QJsonObject &request, QJsonObject project)
{
    const auto root = engineRoot(launcher);
    auto snapshot = request;
    if (snapshot.value("operation") == "synthesize") {
        const auto output = snapshot.value("output_root").toString().isEmpty()
            ? configuredOutputRoot(defaultOutputRoot_) : outputRootForRequest(root_, snapshot);
        snapshot.insert("output_root", output);
    }
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSqlQuery query;
    query.prepare("INSERT INTO jobs(id, created, launcher, operation, state, request)"
                  "VALUES (?, ?, ?, ?, 'queued', ?)");
    query.addBindValue(id);
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(root + "/go-webui.bat");
    query.addBindValue(request.value("operation").toString());
    query.addBindValue(json(snapshot));
    auto database = QSqlDatabase::database();
    if (!database.transaction())
        fail(database.lastError().text());
    try {
        execute(query);
        if (!project.isEmpty()) {
            project.insert("pending", id);
            query.prepare("UPDATE projects SET state=? WHERE id=?");
            query.addBindValue(json(project));
            query.addBindValue(project.value("id").toString());
            execute(query);
            if (query.numRowsAffected() != 1)
                fail(QStringLiteral("训练项目已不存在，请重新打开项目"));
        }
        if (!database.commit())
            fail(database.lastError().text());
    } catch (...) {
        database.rollback();
        throw;
    }
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
        engineLogOffset_ = QFileInfo(checkedPath(root_ + "/engine-start.log")).size();
        const auto operation = activeRequest_.value("operation").toString();
        const auto interfaceIssue = engine_.interfaceIssue(operation);
        if (!interfaceIssue.isEmpty())
            fail(interfaceIssue);
        const auto resourceIssue = jobResourceIssue(engine_.root(), root_, activeRequest_);
        if (!resourceIssue.isEmpty())
            fail(resourceIssue);
        if (operation == "synthesize")
            checkOutputDirectory(outputRootForRequest(root_, activeRequest_));
        update(active_, "running");
        startJobProcess(worker_, engine_.root(), root_, jobPath_, activeRequest_, engine_.webuiPid());
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
    cancelTimeout_.stop();
    QJsonObject result;
    try {
        if (cancelled) {
            removeStagedWeights(id, request);
            const auto message = restartEngine_ ? QStringLiteral("已停止本程序启动的引擎进程")
                                                : QStringLiteral("当前任务已停止");
            update(id, "cancelled", message);
            if (!stopping_) {
                if (restartEngine_)
                    QTimer::singleShot(750, this, &TaskScheduler::continueAfterCancellation);
                else
                    QTimer::singleShot(0, this, &TaskScheduler::next);
            }
            restartEngine_ = false;
            emit this->cancelled(id);
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
                auto outputRoot = outputRootForRequest(root_, request);
                if (!outputRoot.endsWith('/'))
                    outputRoot += '/';
                const auto directory = checkedPath(outputRoot + voice + "/"
                    + QDate::currentDate().toString(Qt::ISODate));
                const auto output = checkedPath(directory + "/" + id + ".wav");
                result.insert("audio", output);
                save(job + "/completion.json", json(result));
                if (!QDir().mkpath(directory))
                    fail(QStringLiteral("无法创建生成目录：%1").arg(directory));
                publishWave(source, output);
            }
            update(id, "succeeded", {}, result);
            emit completed(id, result);
        } else {
            QString detail;
            if (message.isEmpty()) {
                QFile log(checkedPath(job + "/error.log"));
                if (log.open(QIODevice::ReadOnly)) {
                    log.seek(qMax<qint64>(0, log.size() - 4096));
                    detail = QString::fromUtf8(log.readAll()).trimmed();
                    message = detail.section('\n', -1);
                }
                QFile engineLog(checkedPath(root_ + "/engine-start.log"));
                if (engineLog.open(QIODevice::ReadOnly) && engineLog.size() > engineLogOffset_) {
                    engineLog.seek(qMax(engineLogOffset_, engineLog.size() - 16384));
                    detail += '\n' + QString::fromUtf8(engineLog.readAll());
                }
            }
            if (message.isEmpty())
                message = QStringLiteral("任务中断，详情见任务日志");
            message = recoveryHint(message, detail, request.value("operation").toString());
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
    if (id.isEmpty())
        return;
    if (active_ == id) {
        if (cancelling_)
            return;
        cancelling_ = true;
        try {
            save(jobPath_ + "/cancel.requested", {});
            cancelTimeout_.start(cancelGraceMs);
            update(id, "running", QStringLiteral("正在取消任务…"));
        } catch (const std::exception &) {
            forceCancel();
        }
        return;
    }
    QSqlQuery query;
    query.prepare("UPDATE jobs SET state='cancelled' WHERE id=? AND state='queued'");
    query.addBindValue(id);
    execute(query);
    if (query.numRowsAffected() == 1) {
        emit changed();
        emit cancelled(id);
    }
}

void TaskScheduler::forceCancel()
{
    if (!cancelling_ || !running())
        return;
    cancelTimeout_.stop();
    if (engine_.active()) {
        restartEngine_ = true;
        engine_.stop();
    }
    if (worker_.state() != QProcess::NotRunning)
        worker_.kill();
}

void TaskScheduler::continueAfterCancellation()
{
    if (stopping_ || running() || engine_.active())
        return;
    try {
        QSqlQuery query;
        query.prepare("SELECT launcher FROM jobs WHERE state='queued' AND launcher=? COLLATE NOCASE LIMIT 1");
        query.addBindValue(engine_.root() + "/go-webui.bat");
        execute(query);
        if (query.next())
            engine_.start(query.value(0).toString(), root_ + "/engine-start.log");
    } catch (const std::exception &error) {
        emit failed(QStringLiteral("无法恢复排队任务。可在工作台重启引擎。%1").arg(QString::fromUtf8(error.what())));
    }
}

void TaskScheduler::removeStagedWeights(const QString &id, const QJsonObject &request)
{
    const QStringList families{"v2", "v2Pro", "v2ProPlus", "v5dev", "v5turbo"};
    const auto family = request.value("family").toString();
    const auto token = QUuid(id).toString(QUuid::Id128);
    if (request.value("operation") != "synthesize" || !families.contains(family) || QUuid(id).isNull())
        return;
    for (const auto &file : {"/GPT_weights_" + family + "/ez_" + token + ".ckpt",
                             "/SoVITS_weights_" + family + "/ez_" + token + ".pth"}) {
        try {
            QFile::remove(checkedPath(engine_.root() + file));
        } catch (const std::exception &) {
            // Avoid following a path that changed while the task was running.
        }
    }
}

void TaskScheduler::stop()
{
    stopping_ = true;
    if (running()) {
        cancel(active_);
        if (!worker_.waitForFinished(cancelGraceMs)) {
            forceCancel();
            worker_.waitForFinished(1000);
        }
    }
}

bool TaskScheduler::running() const
{
    return !active_.isEmpty();
}

}
