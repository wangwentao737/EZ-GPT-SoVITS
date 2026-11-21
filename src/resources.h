#pragma once

#include <QJsonObject>
#include <QStringList>

namespace ez {

QStringList engineOperations();
QString operationLabel(const QString &operation);
QString resourceIssue(const QString &engine, const QString &operation, const QString &family,
                      const QString &asrLanguage = QStringLiteral("zh"));
QString jobResourceIssue(const QString &engine, const QString &data, const QJsonObject &request);

}
