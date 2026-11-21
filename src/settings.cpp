#include "settings.h"
#include "engine.h"

#include <QSqlError>
#include <QSqlQuery>
#include <stdexcept>

namespace ez {
namespace {

void execute(QSqlQuery &query)
{
    if (!query.exec())
        throw std::runtime_error(query.lastError().text().toStdString());
}

}

void initWorkspaceSettings()
{
    QSqlQuery query;
    query.prepare("CREATE TABLE IF NOT EXISTS workspace_settings (key TEXT PRIMARY KEY, value TEXT NOT NULL)");
    execute(query);
}

QString workspaceSetting(const QString &key)
{
    QSqlQuery query;
    query.prepare("SELECT value FROM workspace_settings WHERE key=?");
    query.addBindValue(key);
    execute(query);
    return query.next() ? query.value(0).toString() : QString();
}

void setWorkspaceSetting(const QString &key, const QString &value)
{
    QSqlQuery query;
    query.prepare("INSERT INTO workspace_settings(key,value) VALUES(?,?) "
                  "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    query.addBindValue(key);
    query.addBindValue(value.isNull() ? QStringLiteral("") : value);
    execute(query);
}

QString configuredOutputRoot(const QString &defaultRoot)
{
    const auto configured = workspaceSetting("outputRoot");
    return checkedWorkspaceRoot(configured.isEmpty() ? defaultRoot : configured);
}

}
