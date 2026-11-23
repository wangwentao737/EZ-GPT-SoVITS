#pragma once

#include <QFutureWatcher>
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <memory>

namespace ez {

inline constexpr char engineBrowserNotification[] = "--engine-browser-notification";

QString checkedPath(const QString &path);
QString engineRoot(const QString &launcher);

struct EngineProbe {
    bool responded = false;
    QString error;
    QMap<QString, QString> unavailable;
    QString devices;
    QString gradio;
};

struct EnginePorts {
    quint16 main = 9874;
    quint16 inference = 9872;
};

EnginePorts enginePorts(const QString &directory);
QString engineFingerprint(const QString &directory);
QString resumeEngineIssue(const QString &directory, const QString &family);

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
    QString interfaceIssue(const QString &operation) const;
    QMap<QString, QString> interfaceIssues() const;
    QString devices() const;
    QString gradioVersion() const;
    EnginePorts ports() const;
    quint32 webuiPid() const;
    bool ownsListener(quint16 port) const;

signals:
    void statusChanged(const QString &message);
    void readinessChanged(bool ready);
    void instanceFailed(const QString &launcher, const QString &message);

private:
    struct Process;
    std::unique_ptr<Process> process_;
    QTimer timer_;
    QFutureWatcher<EngineProbe> probe_;
    QMap<QString, QString> unavailable_;
    QString devices_;
    QString gradio_;
    EnginePorts ports_;
    QString root_;
    QString logPath_;
    int ticks_ = 0;
    bool ready_ = false;
    void poll();
    void failInstance(const QString &message);
};

}
