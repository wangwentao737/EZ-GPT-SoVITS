#pragma once

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QStringList>

class QNetworkAccessManager;

namespace ez {

class EngineManager;

struct EngineInstallation {
    QString directory;
    QString fingerprint;
    QString mainDigest;
    QString inferenceDigest;
    QString adapter;
    QString error;
    QString identityNote;
    QJsonObject entry;
    QMap<QString, QString> interfaces;
    bool connected = false;

    QString package() const;
    QString description() const;
    QString operationFingerprint(const QString &operation, const QString &family, const QString &language) const;
    QString issue(const QString &operation, const QString &family = {}, const QString &language = "zh",
                  bool trial = false) const;
    QStringList families(const QStringList &operations) const;
    QString report(const QString &operation, const QString &family) const;
};

QStringList adapterFamilies(const QString &adapter = {});
QStringList compatibilityOperations();
QString compatibilityOperation(const QJsonObject &request);
QString adapterContractFile(const QString &adapter);
QString defaultEngineAdapter();
QString engineScriptDigest(const QString &directory, const QString &relativePath);
QJsonObject validatedCompatibilityRegistry(const QByteArray &contents);
QStringList supportedEngineAdapters();
QStringList adapterOperations(const QString &adapter);

class CompatibilityRegistry final : public QObject {
    Q_OBJECT
public:
    CompatibilityRegistry(const QString &data, EngineManager &engine);

    EngineInstallation inspect(const QString &launcher) const;
    int revision() const;
    QString install(const QByteArray &contents);
    void checkOnline();
    void recordInferenceIssue(const QString &launcher, const QString &fingerprint, const QString &family,
                              const QString &issue);
    void clearInterfaceIssues(const QString &launcher);

signals:
    void changed();
    void updateFinished(const QString &message);

private:
    QString cache_;
    QJsonObject registry_;
    EngineManager &engine_;
    QMap<QPair<QString, QString>, QMap<QString, QString>> interfaceChecks_;
    QNetworkAccessManager *network_ = nullptr;
    bool downloading_ = false;
};

}
