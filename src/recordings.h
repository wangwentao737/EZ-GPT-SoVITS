#pragma once

#include <QJsonObject>
#include <QStringList>
#include <atomic>

namespace ez {

struct Recording {
    QString source;
    QString name;
    QString originalName;
    QString batch;
    QByteArray digest;
    bool incoming;
};

struct RecordingPlan {
    QList<Recording> files;
    QStringList conflicts;
    QStringList duplicates;
    QStringList replaced;
    int additions = 0;
    QString error;
};

RecordingPlan planRecordings(const QStringList &sources, const QString &current, const QJsonObject &groups,
    const QJsonObject &originalNames, const QString &replace, const QString &batch, const std::atomic_bool &cancel);
QString copyRecordings(const RecordingPlan &plan, const QString &directory, const QString &pendingDirectory,
    const QStringList &pending, const std::atomic_bool &cancel, std::atomic_int &completed);

}
