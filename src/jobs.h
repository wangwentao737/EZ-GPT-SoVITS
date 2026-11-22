#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>

namespace ez {

class EngineManager;

struct JobActivity {
    QString id;
    QString operation;
    QString phase;
    QString device;
    QString unit;
    int completed = 0;
    int total = 0;
    qint64 seconds = 0;

    bool operator==(const JobActivity &) const = default;
};

QString jobActivityText(const JobActivity &activity);

void startJobProcess(QProcess &worker, const QString &engine, const QString &root,
                     const QString &job, QJsonObject request, quint32 webuiPid);

class TaskScheduler final : public QObject {
    Q_OBJECT
public:
    TaskScheduler(QString dataRoot, QString defaultOutputRoot, EngineManager &engine, QObject *parent = nullptr);
    QString enqueue(const QString &launcher, QJsonObject request, const QJsonObject &project = {});
    QStringList enqueueBatch(const QString &launcher, const QList<QJsonObject> &requests);
    QString enqueueSynthesisBatch(const QString &launcher, QList<QJsonObject> requests);
    void startEngine(const QString &launcher);
    void cancel(const QString &id);
    void cancelQueuedBatch(const QString &batch);
    void stop();
    bool running() const;
    const JobActivity &activity() const { return activity_; }

signals:
    void changed();
    void completed(const QString &id, const QJsonObject &result);
    void cancelled(const QString &id);
    void failed(const QString &message);
    void activityChanged();

private:
    void next();
    void pollActivity();
    QStringList enqueueRequests(const QString &launcher, const QList<QJsonObject> &requests, QJsonObject project = {});
    void failQueued(const QString &launcher, const QString &message);
    void finish(bool processSucceeded, QString message);
    void forceCancel();
    void continueAfterCancellation();
    void removeStagedWeights(const QString &id, const QJsonObject &request);
    void update(const QString &id, const QString &state, const QString &message = {},
                const QJsonObject &result = {});

    QString root_;
    QString defaultOutputRoot_;
    QString active_;
    QString jobPath_;
    QString engineFailure_;
    qint64 engineLogOffset_ = 0;
    QJsonObject activeRequest_;
    QJsonObject activeVoiceVersion_;
    EngineManager &engine_;
    QProcess worker_;
    QTimer cancelTimeout_;
    QTimer activityTimer_;
    QElapsedTimer activityClock_;
    JobActivity activity_;
    bool cancelling_ = false;
    bool stopping_ = false;
    bool restartEngine_ = false;
};

}
