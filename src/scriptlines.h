#pragma once

#include <QStringList>
#include <functional>

class QWidget;

namespace ez {

class TaskScheduler;

void editScriptLines(QWidget *parent, TaskScheduler &scheduler, const QString &launcher,
                     const QString &voice, const QStringList &append,
                     const std::function<void(const QString &)> &play);

}
