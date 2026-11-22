#pragma once

#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>

namespace ez {

QJsonObject voiceInputVersion(const QJsonObject &settings, const QString &launcher);
bool sameSynthesisInputs(const QJsonObject &before, const QJsonObject &after);
QJsonObject latestBatchVerification(const QString &voice, const QString &batch);
QSqlQuery voicesWithVerification(const QSqlDatabase &database = QSqlDatabase::database());
QString voiceVerificationStatus(const QJsonObject &settings, const QJsonObject &record,
                                const QString &previousEngine, const QString &selectedEngine);

}
