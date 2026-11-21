#pragma once

#include <QString>

class QWidget;

namespace ez {

bool exportDiagnostics(QWidget *parent, const QString &root, const QString &engineStatus,
                       const QString &resourceReport, const QString &jobId = {});

}
