#include "jobs.h"
#include "engine.h"
#include "resources.h"
#include "settings.h"
#include "workspacepaths.h"
#include "voiceverification.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QUuid>
#include <QtConcurrentRun>
#include <QtEndian>
#include <stdexcept>
#include <utility>
#include <windows.h>

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

QString compatibilityIssue(const EngineInstallation &installation, const QJsonObject &request, bool trial)
{
    const auto operation = compatibilityOperation(request);
    const auto family = request.value("family").toString();
    const auto language = request.value("asr_language").toString("zh");
    const auto issue = installation.issue(operation, family, language, trial);
    if (!issue.isEmpty())
        return issue;
    const auto pinned = request.value("compat_fingerprint").toString();
    const auto package = request.value("compat_package").toString();
    const auto adapter = request.value("compat_adapter").toString();
    if ((!pinned.isEmpty() && pinned != installation.operationFingerprint(operation, family, language))
        || (!package.isEmpty() && package != installation.package())
        || (!adapter.isEmpty() && adapter != installation.adapter))
        return QStringLiteral("提交任务时的整合包构建已变化。请切回原整合包，或按当前构建重新提交。");
    return {};
}

QString outputRootForRequest(const QString &dataRoot, const QJsonObject &request)
{
    const auto saved = request.value("output_root").toString();
    const auto output = checkedPath(saved.isEmpty() ? dataRoot + "/outputs" : saved);
    if (request.value("external_output").toBool() && !QFileInfo(output).exists())
        fail(QStringLiteral("自定义成品目录不可用。请在「设置」中重新定位：%1").arg(output));
    return output;
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

QString partialWavePath(const QString &target)
{
    return target + ".ez-partial";
}

QString stageWave(const QString &source, const QString &partial, qint64 expectedBytes,
                  const QByteArray &expectedHash, const std::atomic_bool &cancelled)
{
    QFile input(source);
    QFile output(partial);
    if (!input.open(QIODevice::ReadOnly) || input.size() != expectedBytes || expectedBytes <= 44
        || expectedHash.size() != 64)
        return QStringLiteral("音频大小或摘要记录无效，请重新生成");
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return QStringLiteral("无法创建音频归档临时文件：%1").arg(output.errorString());
    auto cleanup = qScopeGuard([&output] { output.remove(); });
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 copied = 0;
    while (!cancelled.load()) {
        const auto chunk = input.read(1024 * 1024);
        if (input.error() != QFileDevice::NoError)
            return QStringLiteral("无法读取待归档的音频：%1").arg(input.errorString());
        if (chunk.isEmpty())
            break;
        if (output.write(chunk) != chunk.size())
            return QStringLiteral("音频写入中断，请检查生成目录权限和可用空间：%1").arg(output.errorString());
        hash.addData(chunk);
        copied += chunk.size();
    }
    if (cancelled.load())
        return QStringLiteral("音频归档已取消");
    if (copied != expectedBytes || hash.result().toHex() != expectedHash)
        return QStringLiteral("待归档的音频在保存期间发生变化，请重新生成");
    if (!output.flush())
        return QStringLiteral("无法写入完整的音频：%1").arg(output.errorString());
    output.close();
    cleanup.dismiss();
    return {};
}

}

QString jobActivityText(const JobActivity &activity)
{
    if (activity.id.isEmpty())
        return {};
    auto text = operationLabel(activity.operation) + QStringLiteral(" · ") + activity.phase;
    if (activity.total > 0)
        text += QStringLiteral(" %1/%2 %3").arg(activity.completed).arg(activity.total).arg(activity.unit);
    if (!activity.device.isEmpty())
        text += QStringLiteral(" · %1").arg(activity.device);
    const auto hours = activity.seconds / 3600;
    const auto minutes = activity.seconds / 60 % 60;
    const auto seconds = activity.seconds % 60;
    auto elapsed = QStringLiteral("%1 分 %2 秒").arg(minutes).arg(seconds, 2, 10, QChar('0'));
    if (hours > 0)
        elapsed.prepend(QStringLiteral("%1 时 ").arg(hours));
    return text + QStringLiteral(" · 已用 %1").arg(elapsed);
}

static QJsonObject prepareJob(const EngineManager &engine, const QString &root,
                             const QString &directory, QJsonObject request)
{
    if (!QDir().mkpath(directory))
        fail(QStringLiteral("无法创建任务目录"));
    request.insert("engine", engine.root());
    request.insert("data", root);
    request.insert("job", directory);
    request.insert("webui_pid", static_cast<qint64>(engine.webuiPid()));
    request.insert("main_port", int(engine.ports().main));
    request.insert("inference_port", int(engine.ports().inference));
    request.insert("adapter", engine.adapter());
    save(directory + "/request.json", json(request));
    return request;
}

static QString prepareBridge(const QString &directory, const QString &adapter)
{
    for (const auto &name : QStringList{"bridge.py", adapterContractFile(adapter)}) {
        QFile embedded(QString(":/engine/") + name);
        if (!embedded.open(QIODevice::ReadOnly))
            fail(QStringLiteral("无法读取引擎适配文件 %1").arg(name));
        save(directory + '/' + name, embedded.readAll());
    }
    return directory + "/bridge.py";
}

static void configureWorker(QProcess &worker, const QString &engine)
{
    worker.setWorkingDirectory(engine);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("PATH", QDir::toNativeSeparators(engine + "/runtime") + ';' + environment.value("PATH"));
    worker.setProcessEnvironment(environment);
    worker.setProgram(checkedPath(engine + "/runtime/python.exe"));
}

void startJobProcess(QProcess &worker, const EngineManager &engine, const QString &root,
                     const QString &job, QJsonObject request)
{
    const auto directory = checkedPath(job);
    prepareJob(engine, root, directory, std::move(request));
    const auto bridge = prepareBridge(directory, engine.adapter());
    configureWorker(worker, engine.root());
    worker.setStandardOutputFile(checkedPath(directory + "/result.log"));
    worker.setStandardErrorFile(checkedPath(directory + "/error.log"));
    worker.setArguments({"-I", "-X", "utf8", "-B", "-u", bridge, engine.adapter(), directory + "/request.json"});
    worker.start();
}

TaskScheduler::TaskScheduler(QString dataRoot, QString defaultOutputRoot, EngineManager &engine, QObject *parent)
    : QObject(parent), root_(std::move(dataRoot)), defaultOutputRoot_(std::move(defaultOutputRoot)),
      engine_(engine), compatibility_(root_, engine_)
{
    QSqlQuery query;
    query.prepare("CREATE TABLE IF NOT EXISTS jobs ("
                  "id TEXT PRIMARY KEY, created TEXT NOT NULL, launcher TEXT NOT NULL,"
                  "operation TEXT NOT NULL, state TEXT NOT NULL, request BLOB NOT NULL,"
                  "message TEXT NOT NULL DEFAULT '', result BLOB)");
    execute(query);
    query.prepare("CREATE INDEX IF NOT EXISTS jobs_created ON jobs(created)");
    execute(query);
    query.prepare("CREATE INDEX IF NOT EXISTS jobs_batch ON jobs(json_extract(CAST(request AS TEXT),'$.batch_id'))"
                  " WHERE json_valid(CAST(request AS TEXT))");
    execute(query);
    query.prepare("CREATE INDEX IF NOT EXISTS jobs_script_line ON jobs(json_extract(CAST(request AS TEXT),'$.line_id'))"
                  " WHERE operation='synthesize' AND json_valid(CAST(request AS TEXT))");
    execute(query);
    query.prepare("SELECT id,request FROM jobs WHERE state='running' AND operation='synthesize'");
    execute(query);
    QList<QPair<QString, QJsonObject>> unfinished;
    while (query.next())
        unfinished.append({query.value(0).toString(), QJsonDocument::fromJson(query.value(1).toByteArray()).object()});
    query.finish();
    for (const auto &[id, snapshot] : unfinished) {
        try {
            if (QUuid(id).isNull())
                continue;
            const auto request = resolvedWorkspacePaths(snapshot);
            auto receipt = readResult(root_ + "/jobs/" + id + "/completion.json");
            if (request.value("external_output").toBool())
                receipt["external_output"] = true;
            const auto completion = resolvedWorkspacePaths(receipt);
            const auto audio = completion.value("audio").toString();
            const auto output = outputRootForRequest(root_, request);
            if (!audio.startsWith(output.endsWith('/') ? output : output + '/', Qt::CaseInsensitive)
                || QDir::cleanPath(audio) != audio
                || QFileInfo(audio).fileName() != id + ".wav")
                continue;
            if (!QUuid(id).isNull() && completion.value("archive_version").toInt() == 1)
                QFile::remove(checkedPath(partialWavePath(audio)));
            if (isArchivedWave(audio, completion))
                update(id, "succeeded", {}, completion);
        } catch (const std::exception &) {
            // An unavailable artifact stays in the interrupted task list.
        }
    }
    query.prepare("UPDATE jobs SET state='interrupted' WHERE state='running'");
    execute(query);
    activityTimer_.setInterval(300);
    connect(&activityTimer_, &QTimer::timeout, this, &TaskScheduler::pollActivity);
    connect(&archive_, &QFutureWatcher<QString>::finished, this, &TaskScheduler::finishArchive);
    cancelTimeout_.setSingleShot(true);
    connect(&cancelTimeout_, &QTimer::timeout, this, &TaskScheduler::forceCancel);
    inferenceTimeout_.setSingleShot(true);
    connect(&inferenceTimeout_, &QTimer::timeout, &inferenceWorker_, &QProcess::kill);
    connect(&inferenceWorker_, &QProcess::readyReadStandardOutput, this, &TaskScheduler::readInferenceStatus);
    connect(&inferenceWorker_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        inferenceExited(code == 0 && status == QProcess::NormalExit);
    });
    connect(&inferenceWorker_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            inferenceExited(false);
    });
    connect(&engine_, &EngineManager::readinessChanged, this, [this](bool ready) {
        if (ready) {
            next();
        } else if (!cancelling_) {
            if (worker_.state() != QProcess::NotRunning)
                worker_.kill();
            if (inferenceWorker_.state() != QProcess::NotRunning)
                inferenceWorker_.kill();
        }
    });
    connect(&engine_, &EngineManager::instanceFailed, this, [this](const QString &launcher, const QString &message) {
        if (running() && archiveReceipt_.isEmpty() && !cancelling_)
            engineFailure_ = message;
        failQueued(launcher, message);
        QTimer::singleShot(0, this, &TaskScheduler::next);
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

TaskScheduler::~TaskScheduler()
{
    stop();
    if (!archiveReceipt_.isEmpty()) {
        archiveCancelled_.store(true);
        archive_.waitForFinished();
        if (archive_.result().isEmpty())
            QFile::remove(partialWavePath(archiveReceipt_.value("audio").toString()));
    }
}

QString TaskScheduler::enqueue(const QString &launcher, QJsonObject request, const QJsonObject &project)
{
    request.remove("batch_id");
    request.remove("batch_index");
    request.remove("batch_size");
    return enqueueRequests(launcher, {request}, project).constFirst();
}

QString TaskScheduler::enqueueTrial(const QString &launcher, const QJsonObject &request)
{
    return enqueueRequests(launcher, {request}, {}, true).constFirst();
}

QString TaskScheduler::voiceIssue(const EngineInstallation &installation, QJsonObject voice, bool trial) const
{
    try {
        voice["operation"] = "synthesize";
        const auto issue = compatibilityIssue(installation, voice, trial);
        return issue.isEmpty() ? jobResourceIssue(installation.directory, root_, voice) : issue;
    } catch (const std::exception &error) {
        return QString::fromUtf8(error.what());
    }
}

QStringList TaskScheduler::enqueueBatch(const QString &launcher, const QList<QJsonObject> &requests)
{
    return enqueueRequests(launcher, requests);
}

QString TaskScheduler::enqueueSynthesisBatch(const QString &launcher, QList<QJsonObject> requests)
{
    const auto batch = QUuid::createUuid().toString(QUuid::WithoutBraces);
    for (qsizetype index = 0; index < requests.size(); ++index) {
        requests[index]["batch_id"] = batch;
        requests[index]["batch_index"] = index + 1;
        requests[index]["batch_size"] = requests.size();
    }
    enqueueRequests(launcher, requests);
    return batch;
}

QStringList TaskScheduler::enqueueRequests(const QString &launcher, const QList<QJsonObject> &requests,
                                           QJsonObject project, bool trial)
{
    if (requests.isEmpty())
        fail(QStringLiteral("没有可提交的任务"));
    const auto entry = checkedPath(resolvedEngineLauncher(launcher));
    const auto installation = compatibility_.inspect(entry);
    const auto created = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QStringList submitted;
    auto database = QSqlDatabase::database();
    if (!database.transaction())
        fail(database.lastError().text());
    try {
        QSqlQuery query;
        query.prepare("INSERT INTO jobs(id, created, launcher, operation, state, request, message, result)"
                      "VALUES (?, ?, ?, ?, ?, ?, ?, '{}')");
        for (auto snapshot : requests) {
            const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const auto operation = snapshot.value("operation").toString();
            if (operation.isEmpty())
                fail(QStringLiteral("任务缺少操作类型"));
            const auto issue = operation == "synthesize" ? voiceIssue(installation, resolvedWorkspacePaths(snapshot), trial)
                : compatibilityIssue(installation, snapshot, trial);
            if (!issue.isEmpty() && (!project.isEmpty() || trial))
                fail(issue);
            snapshot["compatibility_trial"] = trial;
            if (!snapshot.contains("compat_fingerprint"))
                snapshot["compat_fingerprint"] = installation.operationFingerprint(compatibilityOperation(snapshot),
                    snapshot.value("family").toString(), snapshot.value("asr_language").toString("zh"));
            if (!snapshot.contains("compat_package"))
                snapshot["compat_package"] = installation.package();
            if (!snapshot.contains("compat_adapter"))
                snapshot["compat_adapter"] = installation.adapter;
            if (operation == "synthesize") {
                const auto savedOutput = snapshot.value("output_root").toString();
                const auto output = savedOutput.isEmpty() ? configuredOutputRoot(defaultOutputRoot_)
                    : outputRootForRequest(root_, resolvedWorkspacePaths(snapshot));
                snapshot["output_root"] = output;
                if (savedOutput.isEmpty() || !snapshot.contains("external_output"))
                    snapshot["external_output"] = output.compare(defaultOutputRoot_, Qt::CaseInsensitive) != 0;
            }
            query.bindValue(0, id);
            query.bindValue(1, created);
            query.bindValue(2, entry);
            query.bindValue(3, operation);
            query.bindValue(4, issue.isEmpty() ? "queued" : "failed");
            query.bindValue(5, json(storedWorkspacePaths(snapshot)));
            query.bindValue(6, issue.isEmpty() ? QStringLiteral("") : issue);
            execute(query);
            submitted.append(id);
        }
        if (!project.isEmpty()) {
            project.insert("pending", submitted.constFirst());
            query.prepare("UPDATE projects SET state=? WHERE id=?");
            query.addBindValue(json(storedWorkspacePaths(project)));
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
    next();
    return submitted;
}

void TaskScheduler::startEngine(const QString &launcher)
{
    try {
        const auto installation = compatibility_.inspect(launcher);
        engine_.start(launcher, root_ + "/engine-start.log", installation.adapter);
    } catch (const std::exception &error) {
        failQueued(launcher, QString::fromUtf8(error.what()));
        QTimer::singleShot(0, this, &TaskScheduler::next);
    }
}

void TaskScheduler::failQueued(const QString &launcher, const QString &message)
{
    const auto reason = QStringLiteral("引擎不可用：%1\n请检查引擎设置或启动日志，修复后重试。").arg(message);
    try {
        QSqlQuery query;
        query.prepare("UPDATE jobs SET state='failed',message=?,result='{}'"
                      " WHERE state='queued' AND launcher=? COLLATE NOCASE");
        query.addBindValue(reason);
        query.addBindValue(launcher);
        execute(query);
        if (query.numRowsAffected() > 0)
            emit changed();
        emit failed(reason);
    } catch (const std::exception &error) {
        emit failed(QStringLiteral("无法更新等待引擎的任务：%1").arg(QString::fromUtf8(error.what())));
    }
}

void TaskScheduler::next()
{
    if (stopping_ || running() || inferenceClosing_ || (engine_.active() && !engine_.ready()))
        return;
    try {
        QSqlQuery query;
        query.prepare("SELECT id, request, launcher FROM jobs WHERE state='queued' ORDER BY created, rowid LIMIT 1");
        execute(query);
        if (!query.next()) {
            closeInference();
            return;
        }
        const auto id = query.value(0).toString();
        const auto snapshot = query.value(1).toByteArray();
        const auto launcher = query.value(2).toString();
        query.finish();
        const auto queued = QJsonDocument::fromJson(snapshot).object();
        const auto operation = queued.value("operation").toString();
        const auto batch = queued.value("batch_id").toString();
        const auto voice = queued.value("voice_id").toString();
        const auto group = operation != "synthesize" ? QString()
            : batch.isEmpty() || voice.isEmpty() ? id : batch + '/' + voice;
        const auto installation = compatibility_.inspect(launcher);
        const bool reconnect = !installation.directory.isEmpty() && !installation.connected;
        if (!inferenceGroup_.isEmpty() && (inferenceGroup_ != group || reconnect)) {
            closeInference();
            return;
        }
        active_ = id;
        if (QUuid(id).isNull())
            fail(QStringLiteral("任务编号无效"));
        activeRequest_ = readWorkspaceRecord(snapshot);
        jobPath_ = checkedPath(root_ + "/jobs/" + active_);
        engineLogOffset_ = QFileInfo(checkedPath(root_ + "/engine-start.log")).size();
        const auto issue = compatibilityIssue(installation, activeRequest_, activeRequest_.value("compatibility_trial").toBool());
        if (!issue.isEmpty())
            fail(issue);
        if (reconnect) {
            active_.clear();
            activeRequest_ = {};
            if (engine_.active())
                engine_.stop();
            if (engine_.canStart())
                startEngine(launcher);
            else
                QTimer::singleShot(250, this, &TaskScheduler::next);
            return;
        }
        const auto resourceIssue = jobResourceIssue(engine_.root(), root_, activeRequest_);
        if (!resourceIssue.isEmpty())
            fail(resourceIssue);
        auto payload = activeRequest_;
        activeEngineFingerprint_ = installation.fingerprint;
        if (operation == "synthesize") {
            checkOutputDirectory(outputRootForRequest(root_, activeRequest_));
            const auto launcher = engine_.root() + "/go-webui.bat";
            activeVoiceVersion_ = voiceInputVersion(activeRequest_, launcher);
            if (!activeVoiceVersion_.isEmpty()) {
                payload["record_voice_verification"] = true;
                const auto previous = latestBatchVerification(activeRequest_.value("voice_id").toString(),
                                                               activeRequest_.value("batch_id").toString());
                if (previous.value("input_version").toObject() == activeVoiceVersion_)
                    payload["verification_hashes"] = previous.value("sha256");
            }
        }
        update(active_, "running");
        activityClock_.start();
        pollActivity();
        activityTimer_.start();
        if (operation == "synthesize")
            submitInference(std::move(payload), group);
        else
            startJobProcess(worker_, engine_, root_, jobPath_, std::move(payload));
    } catch (const std::exception &error) {
        finish(false, QString::fromUtf8(error.what()));
    }
}

void TaskScheduler::submitInference(QJsonObject request, const QString &group)
{
    request = prepareJob(engine_, root_, jobPath_, std::move(request));
    if (inferenceWorker_.state() == QProcess::NotRunning) {
        const auto bridge = prepareBridge(jobPath_, engine_.adapter());
        configureWorker(inferenceWorker_, engine_.root());
        inferenceWorker_.setStandardErrorFile(checkedPath(jobPath_ + "/inference-worker.log"));
        inferenceWorker_.setArguments({"-I", "-X", "utf8", "-B", "-u", bridge, engine_.adapter(), "--synthesis-worker"});
        inferenceWorker_.start();
    }
    inferenceGroup_ = group;
    inferenceTask_ = true;
    inferenceStaged_.insert(active_, activeRequest_);
    const auto content = json(request) + '\n';
    if (inferenceWorker_.write(content) != content.size())
        inferenceWorker_.kill();
}

void TaskScheduler::readInferenceStatus()
{
    inferenceStatus_ += inferenceWorker_.readAllStandardOutput();
    qsizetype end;
    while ((end = inferenceStatus_.indexOf('\n')) >= 0) {
        const auto fields = inferenceStatus_.left(end).simplified().split(' ');
        inferenceStatus_.remove(0, end + 1);
        if (!inferenceTask_ || fields.size() != 3 || fields[0] != "EZ_DONE" || fields[1] != active_.toLatin1())
            continue;
        bool valid = false;
        const int code = fields[2].toInt(&valid);
        if (!valid || (code != 0 && code != 1 && code != cancelledExitCode)) {
            inferenceWorker_.kill();
            return;
        }
        inferenceTask_ = false;
        finish(code == 0, {});
    }
}

void TaskScheduler::closeInference()
{
    if (inferenceWorker_.state() == QProcess::NotRunning || inferenceClosing_ || inferenceTask_)
        return;
    inferenceClosing_ = true;
    inferenceTimeout_.start(cancelGraceMs);
    inferenceWorker_.closeWriteChannel();
}

void TaskScheduler::inferenceExited(bool success)
{
    readInferenceStatus();
    const bool taskLost = std::exchange(inferenceTask_, false);
    const bool closed = std::exchange(inferenceClosing_, false);
    inferenceTimeout_.stop();
    inferenceGroup_.clear();
    inferenceStatus_.clear();
    if (!closed || !success || taskLost) {
        if (engine_.active()) {
            restartEngine_ = true;
            engine_.stop();
        }
        for (auto item = inferenceStaged_.constBegin(); item != inferenceStaged_.constEnd(); ++item)
            removeStagedWeights(item.key(), item.value());
    }
    inferenceStaged_.clear();
    if (taskLost) {
        finish(false, {});
    } else if (!running() && !stopping_) {
        if (std::exchange(restartEngine_, false))
            QTimer::singleShot(750, this, &TaskScheduler::continueAfterCancellation);
        else
            QTimer::singleShot(0, this, &TaskScheduler::next);
    }
}

void TaskScheduler::pollActivity()
{
    if (!running() || !activityClock_.isValid())
        return;
    JobActivity next;
    next.id = active_;
    next.operation = activeRequest_.value("operation").toString();
    next.seconds = activityClock_.elapsed() / 1000;
    next.phase = QStringLiteral("正在启动任务");
    QFile file(jobPath_ + "/progress.json");
    if (file.open(QIODevice::ReadOnly) && file.size() <= 4096) {
        const auto progress = QJsonDocument::fromJson(file.read(4096)).object();
        const auto phase = progress.value("phase").toString();
        if (!phase.isEmpty())
            next.phase = phase.left(100);
        next.completed = progress.value("completed").toInt();
        next.total = progress.value("total").toInt();
        next.unit = progress.value("unit").toString().left(8);
        next.device = progress.value("device").toString().left(24);
        if (next.completed < 0 || next.total < next.completed || next.unit.isEmpty())
            next.total = 0;
    } else if (activity_.id == active_) {
        next.phase = activity_.phase;
        next.completed = activity_.completed;
        next.total = activity_.total;
        next.unit = activity_.unit;
        next.device = activity_.device;
    }
    if (next.device.isEmpty()) {
        const auto gpu = activeRequest_.value("gpu").toString();
        if (!gpu.isEmpty())
            next.device = QStringLiteral("所选显卡 %1").arg(gpu.left(2));
    }
    if (!archiveReceipt_.isEmpty()) {
        next.phase = QStringLiteral("正在归档 WAV");
        next.total = 0;
    }
    if (cancelling_) {
        next.phase = QStringLiteral("正在取消任务");
        next.total = 0;
    }
    if (activity_ != next) {
        activity_ = next;
        emit activityChanged();
    }
}

void TaskScheduler::finish(bool processSucceeded, QString message)
{
    if (!running())
        return;
    if (cancelling_) {
        removeStagedWeights(active_, activeRequest_);
        settle({});
        return;
    }
    try {
        if (processSucceeded) {
            QJsonObject result;
            QFile log(checkedPath(jobPath_ + "/result.log"));
            if (log.open(QIODevice::ReadOnly)) {
                while (!log.atEnd()) {
                    const auto line = log.readLine();
                    if (line.startsWith("EZ_RESULT "))
                        result = QJsonDocument::fromJson(line.mid(10)).object();
                }
            }
            if (result.isEmpty())
                fail(QStringLiteral("引擎未提交有效的任务产物"));
            result["elapsed_ms"] = activityClock_.isValid() ? activityClock_.elapsed() : 0;
            if (activeEngineFingerprint_ == engineFingerprint(engine_.root())) {
                result["engine_fingerprint"] = activeEngineFingerprint_;
                result["gradio_version"] = engine_.gradioVersion();
            }
            if (activeRequest_.value("operation") == "synthesize") {
                startArchive(std::move(result));
                return;
            }
            settle(result);
            return;
        }
        QString detail;
        if (message.isEmpty()) {
            QFile log(checkedPath(jobPath_ + "/error.log"));
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
        const auto conflict = readResult(jobPath_ + "/interface-error.json").value("issue").toString();
        if (!conflict.isEmpty() && activeRequest_.value("operation") == "synthesize") {
            compatibility_.recordInferenceIssue(engine_.root() + "/go-webui.bat", activeEngineFingerprint_,
                                                activeRequest_.value("family").toString(), conflict);
            message = conflict;
        }
        if (!engineFailure_.isEmpty())
            message = engineFailure_;
        if (message.isEmpty())
            message = QStringLiteral("任务中断，详情见任务日志");
        message = recoveryHint(message, detail, activeRequest_.value("operation").toString());
    } catch (const std::exception &error) {
        message = QString::fromUtf8(error.what());
    }
    settle({}, message);
}

void TaskScheduler::startArchive(QJsonObject result)
{
    const auto hashes = result.take("verification_hashes").toObject();
    if (!activeVoiceVersion_.isEmpty()) {
        const auto launcher = engine_.root() + "/go-webui.bat";
        const auto current = voiceInputVersion(activeRequest_, launcher);
        if (!sameSynthesisInputs(activeVoiceVersion_, current))
            fail(QStringLiteral("合成期间权重、参考音频或引擎文件发生变化。请确认文件后重试。"));
        if (hashes.size() == 3) {
            result["voice_verification"] = QJsonObject{
                {"launcher", launcher}, {"input_version", current}, {"sha256", hashes},
                {"verified_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                {"parameters", result.value("parameters")}, {"seed", result.value("seed")},
                {"language", activeRequest_.value("language").toString(QStringLiteral("中文"))},
            };
        }
    }
    const auto voice = QUuid(activeRequest_.value("voice_id").toString()).toString(QUuid::WithoutBraces);
    const auto outputRoot = outputRootForRequest(root_, activeRequest_);
    const auto directory = checkedPath(QDir(outputRoot).filePath(
        voice + "/" + QDate::currentDate().toString(Qt::ISODate)));
    const auto output = checkedPath(directory + "/" + active_ + ".wav");
    const auto partial = checkedPath(partialWavePath(output));
    const auto source = checkedPath(jobPath_ + "/verified.wav");
    if (!QDir().mkpath(directory))
        fail(QStringLiteral("无法创建生成目录：%1").arg(directory));
    if (QFileInfo::exists(partial) || QFileInfo::exists(output))
        fail(QStringLiteral("此任务的归档路径已存在，请重试任务"));
    result["audio"] = output;
    if (activeRequest_.value("external_output").toBool())
        result["external_output"] = true;
    result["archive_version"] = 1;
    save(jobPath_ + "/completion.json", json(storedWorkspacePaths(result)));

    const auto bytes = result.value("bytes").toInteger();
    const auto hash = result.value("sha256").toString().toLatin1();
    archiveCancelled_.store(false);
    archiveReceipt_ = std::move(result);
    archive_.setFuture(QtConcurrent::run([this, source, partial, bytes, hash] {
        return stageWave(source, partial, bytes, hash, archiveCancelled_);
    }));
    pollActivity();
}

void TaskScheduler::finishArchive()
{
    if (archiveReceipt_.isEmpty())
        return;
    auto result = std::exchange(archiveReceipt_, {});
    auto message = archive_.result();
    if (message.isEmpty()) {
        try {
            const auto output = checkedPath(result.value("audio").toString());
            const auto partial = checkedPath(partialWavePath(output));
            if (!cancelling_) {
                const auto target = QDir::toNativeSeparators(output);
                const auto staged = QDir::toNativeSeparators(partial);
                // Both paths share a parent. Rename atomically without replacing a destination.
                if (MoveFileExW(reinterpret_cast<LPCWSTR>(staged.utf16()),
                                reinterpret_cast<LPCWSTR>(target.utf16()), MOVEFILE_WRITE_THROUGH)) {
                    QFile::remove(jobPath_ + "/verified.wav");
                    result["elapsed_ms"] = activityClock_.elapsed();
                    settle(result);
                    return;
                }
                message = QStringLiteral("无法保存成品音频，请检查生成目录权限（Windows %1）").arg(GetLastError());
            }
            QFile::remove(partial);
        } catch (const std::exception &error) {
            message = QString::fromUtf8(error.what());
        }
    }
    settle({}, message);
}

void TaskScheduler::settle(const QJsonObject &result, const QString &message)
{
    const auto id = std::exchange(active_, {});
    jobPath_.clear();
    activeRequest_ = {};
    activeVoiceVersion_ = {};
    activeEngineFingerprint_.clear();
    engineFailure_.clear();
    const bool cancelled = std::exchange(cancelling_, false);
    const bool restart = std::exchange(restartEngine_, false);
    cancelTimeout_.stop();
    activityTimer_.stop();
    activityClock_.invalidate();
    if (cancelled)
        closeInference();
    if (!activity_.id.isEmpty()) {
        activity_ = {};
        emit activityChanged();
    }
    try {
        if (cancelled) {
            update(id, "cancelled", restart ? QStringLiteral("已停止本程序启动的引擎进程")
                                            : QStringLiteral("当前任务已停止"));
            emit this->cancelled(id);
        } else if (result.isEmpty()) {
            update(id, "failed", message);
            emit failed(message);
        } else {
            update(id, "succeeded", {}, result);
            emit completed(id, result);
        }
    } catch (const std::exception &error) {
        // A committed artifact can still be recovered from its receipt on startup.
        emit failed(QString::fromUtf8(error.what()));
    }
    if (!stopping_) {
        if (restart)
            QTimer::singleShot(750, this, &TaskScheduler::continueAfterCancellation);
        else
            QTimer::singleShot(0, this, &TaskScheduler::next);
    }
}

void TaskScheduler::update(const QString &id, const QString &state, const QString &message,
                           const QJsonObject &result)
{
    QSqlQuery query;
    query.prepare("UPDATE jobs SET state=?, message=?, result=? WHERE id=?");
    query.addBindValue(state);
    query.addBindValue(message.isNull() ? QStringLiteral("") : message);
    query.addBindValue(json(storedWorkspacePaths(result)));
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
        if (!archiveReceipt_.isEmpty()) {
            archiveCancelled_.store(true);
            update(id, "running", QStringLiteral("正在取消归档…"));
            pollActivity();
            return;
        }
        try {
            save(jobPath_ + "/cancel.requested", {});
            cancelTimeout_.start(cancelGraceMs);
            update(id, "running", QStringLiteral("正在取消任务…"));
            pollActivity();
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

void TaskScheduler::cancelQueuedBatch(const QString &batch)
{
    if (batch.isEmpty())
        return;
    QSqlQuery query;
    query.prepare("UPDATE jobs SET state='cancelled',message=? WHERE state='queued'"
                  " AND json_extract(CAST(request AS TEXT),'$.batch_id')=?");
    query.addBindValue(QStringLiteral("已取消本批等待项。"));
    query.addBindValue(batch);
    execute(query);
    if (query.numRowsAffected() > 0)
        emit changed();
}

void TaskScheduler::forceCancel()
{
    if (!cancelling_ || !running() || !archiveReceipt_.isEmpty())
        return;
    cancelTimeout_.stop();
    if (engine_.active()) {
        restartEngine_ = true;
        engine_.stop();
    }
    if (worker_.state() != QProcess::NotRunning)
        worker_.kill();
    if (inferenceWorker_.state() != QProcess::NotRunning)
        inferenceWorker_.kill();
}

void TaskScheduler::continueAfterCancellation()
{
    next();
}

void TaskScheduler::removeStagedWeights(const QString &id, const QJsonObject &request)
{
    const auto families = adapterFamilies();
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
        if (!archiveReceipt_.isEmpty()) {
            archive_.waitForFinished();
            finishArchive();
        } else if (inferenceTask_ && !inferenceWorker_.waitForReadyRead(cancelGraceMs)) {
            forceCancel();
        } else if (worker_.state() != QProcess::NotRunning && !worker_.waitForFinished(cancelGraceMs)) {
            forceCancel();
            worker_.waitForFinished(1000);
        }
    }
    if (inferenceTask_)
        forceCancel();
    closeInference();
    if (inferenceWorker_.state() != QProcess::NotRunning && !inferenceWorker_.waitForFinished(cancelGraceMs)) {
        inferenceWorker_.kill();
        inferenceWorker_.waitForFinished(1000);
    }
}

bool TaskScheduler::running() const
{
    return !active_.isEmpty();
}

}
