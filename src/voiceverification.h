#pragma once

#include <QJsonObject>

namespace ez {

QJsonObject voiceInputVersion(const QJsonObject &settings, const QString &launcher);
bool sameSynthesisInputs(const QJsonObject &before, const QJsonObject &after);

}
