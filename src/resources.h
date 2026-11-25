#pragma once

#include <QJsonObject>
#include <QStringList>
#include <atomic>

namespace ez {

struct EngineInstallation;

QStringList engineOperations();
QString operationLabel(const QString &operation);
bool resourceRuleIncludesScript(const QString &operation, const QString &family,
                                const QString &language, const QString &script);
QString resourceIssue(const QString &engine, const QString &operation, const QString &family,
                      const QString &asrLanguage = QStringLiteral("zh"), const std::atomic_bool *cancelled = nullptr);
QString jobResourceIssue(const QString &engine, const QString &data, const QJsonObject &request);
bool readableTrainingCheckpoint(const QString &path);

struct ResourceCheck {
    QString id;
    QString label;
    QString detail;
    bool passed;
};

QList<ResourceCheck> inspectResources(const QString &launcher, const QString &family, const QString &data,
                                      const QString &output, const QJsonObject &voice,
                                      const EngineInstallation &installation, const QString &only,
                                      const std::atomic_bool &cancelled);

}
