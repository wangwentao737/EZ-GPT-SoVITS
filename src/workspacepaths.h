#pragma once

#include <QJsonObject>
#include <QStringList>
#include <functional>

namespace ez {

// Open once, with the workspace lock held, before constructing any panels.
bool openWorkspacePaths(const QString &root, const std::function<bool(const QString &)> &confirm);
QJsonObject storedWorkspacePaths(QJsonObject object);
QJsonObject resolvedWorkspacePaths(QJsonObject object);
QJsonObject readWorkspaceRecord(const QByteArray &json);
QJsonObject readWorkspaceResult(const QByteArray &json, const QJsonObject &request);
QString resolvedWorkspacePath(const QString &path);
QString resolvedEngineLauncher(const QString &launcher);
QString resolvedOutputDirectory(const QString &directory);
QStringList workspaceOutputDirectories();
QString nextArtifactPath(const QString &directory, const QString &type, const QString &extension = {},
                         const QString &temporarySuffix = {});
bool relocateWorkspaceEngine(const QString &previous, const QString &selected,
    const std::function<bool(const QString &)> &confirm);
bool relocateWorkspaceOutput(const QString &previous, const QString &selected,
    const std::function<bool(const QString &)> &confirm);

}
