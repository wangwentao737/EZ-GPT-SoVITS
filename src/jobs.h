#pragma once

#include <QJsonObject>
#include <QObject>
#include <QProcess>

namespace ez {

class EngineManager;

void startJobProcess(QProcess &worker, const QString &engine, const QString &root,
                     const QString &job, QJsonObject request);

class TaskScheduler final : public QObject {
    Q_OBJECT
public:
    TaskScheduler(QString dataRoot, EngineManager &engine, QObject *parent = nullptr);
    QString enqueue(const QString &launcher, const QJsonObject &request);
    void cancel(const QString &id);
    void stop();
    bool running() const;

signals:
    void changed();
    void completed(const QString &id, const QJsonObject &result);
    void failed(const QString &message);

private:
    void next();
    void finish(bool processSucceeded, QString message);
    void update(const QString &id, const QString &state, const QString &message = {},
                const QJsonObject &result = {});

    QString root_;
    QString active_;
    QString jobPath_;
    QJsonObject activeRequest_;
    EngineManager &engine_;
    QProcess worker_;
    bool cancelling_ = false;
    bool stopping_ = false;
};

}
