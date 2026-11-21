#pragma once

#include <QString>

class QTabWidget;
class QWidget;

namespace ez {

class SynthesisPanel;
class TaskScheduler;

QWidget *createLibraryPage(const QString &root, const QString &defaultOutput, TaskScheduler &scheduler,
                           SynthesisPanel *synthesis, QTabWidget *tabs);

}
