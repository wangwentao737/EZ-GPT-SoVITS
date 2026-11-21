#pragma once

#include <QString>

namespace ez {

void initWorkspaceSettings();
QString workspaceSetting(const QString &key);
void setWorkspaceSetting(const QString &key, const QString &value);
QString configuredOutputRoot(const QString &defaultRoot);

}
