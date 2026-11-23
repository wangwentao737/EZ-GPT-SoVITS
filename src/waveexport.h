#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <atomic>

namespace ez {

struct WaveExportItem {
    QString source;
    QString sha256;
    QString filename;
};

QString exportOrderedWaves(const QList<WaveExportItem> &items, const QString &directory,
                          const QSet<QString> &overwrite, const std::atomic_bool &cancel);
QString exportMergedWave(const QList<WaveExportItem> &items, const QString &filename, int gapMs,
                         bool overwrite, const std::atomic_bool &cancel);

}
