#pragma once

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>

namespace ez {

class EngineManager;

void startJobProcess(QProcess &worker, const QString &engine, const QString &root,
                     const QString &job, QJsonObject request);

class TaskScheduler final : public QObject {
    Q_OBJECT
public:
    TaskScheduler(QString dataRoot, QString defaultOutputRoot, EngineManager &engine, QObject *parent = nullptr);
    QString enqueue(const QString &launcher, const QJsonObject &request, QJsonObject project = {});
    void cancel(const QString &id);
    void stop();
    bool running() const;

signals:
    void changed();
    void completed(const QString &id, const QJsonObject &result);
    void cancelled(const QString &id);
    void failed(const QString &message);

private:
    void next();
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
    QJsonObject activeRequest_;
    EngineManager &engine_;
    QProcess worker_;
    QTimer cancelTimeout_;
    bool cancelling_ = false;
    bool stopping_ = false;
    bool restartEngine_ = false;
};

}
