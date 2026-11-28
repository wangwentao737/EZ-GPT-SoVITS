#pragma once

#include "engine.h"

namespace ez::adaptation {

struct Check {
    QString operation;
    QString family;
    QString language;
    QString issue;
    QJsonObject scripts;

    QString label() const;
    QString anchor() const;
};

struct Capture {
    QString directory;
    QString adapter;
    QString fingerprint;
    QString inferenceIssue;
    EnginePorts ports;
    QJsonObject identity;
    EngineInterfaceSnapshot main;
    EngineInterfaceSnapshot inference;
    QList<Check> checks;
};

struct Entry {
    int checkIndex;
    QString result;
    QString evidence;
};

struct Draft {
    QString id;
    QString name;
    QString source;
    QJsonObject baseline;
    QList<Entry> entries;
};

Capture scan(const QString &directory, const QString &adapter, bool captureInference);
QJsonObject loadBaseline(const QString &path);
void validateDraft(const Capture &capture, const Draft &draft, bool localTest);
QString exportDraft(const Capture &capture, const Draft &draft, const QString &parent, bool localTest);

}
