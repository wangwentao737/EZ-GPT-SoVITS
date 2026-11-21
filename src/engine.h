#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <memory>

namespace ez {

QString checkedPath(const QString &path);
QString checkedWorkspaceRoot(const QString &path);
QString engineRoot(const QString &launcher);
QStringList inspectEngine(const QString &launcher, const QString &family);

class EngineManager final : public QObject {
    Q_OBJECT
public:
    explicit EngineManager(QObject *parent = nullptr);
    ~EngineManager() override;
    void start(const QString &launcher, const QString &logPath);
    void stop();
    bool ready() const;
    bool active() const;
    QString root() const;
    bool ownsListener(quint16 port) const;

signals:
    void statusChanged(const QString &message);
    void readinessChanged(bool ready);

private:
    struct Process;
    std::unique_ptr<Process> process_;
    QTimer timer_;
    QFutureWatcher<QString> probe_;
    QString root_;
    QString logPath_;
    int ticks_ = 0;
    bool ready_ = false;
    void poll();
};

}
