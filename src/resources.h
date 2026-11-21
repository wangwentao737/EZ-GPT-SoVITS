#pragma once

#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <atomic>

namespace ez {

QStringList engineOperations();
QString operationLabel(const QString &operation);
QString resourceIssue(const QString &engine, const QString &operation, const QString &family,
                      const QString &asrLanguage = QStringLiteral("zh"), const std::atomic_bool *cancelled = nullptr);
QString jobResourceIssue(const QString &engine, const QString &data, const QJsonObject &request);

struct ResourceCheck {
    QString id;
    QString label;
    QString detail;
    bool passed;
};

QList<ResourceCheck> inspectResources(const QString &launcher, const QString &family, const QString &data,
                                      const QString &output, const QJsonObject &voice,
                                      const QMap<QString, QString> &interfaces, const QString &only,
                                      const std::atomic_bool &cancelled);

}
