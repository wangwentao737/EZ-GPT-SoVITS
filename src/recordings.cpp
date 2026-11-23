#include "recordings.h"
#include "engine.h"

#include <windows.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <stdexcept>

namespace ez {
namespace {

void require(bool ok, const QString &message)
{
    if (!ok)
        throw std::runtime_error(message.toStdString());
}

void checkCancel(const std::atomic_bool &cancel)
{
    require(!cancel.load(), QStringLiteral("已取消导入录音。"));
}

QByteArray sha256(const QString &path, const std::atomic_bool &cancel)
{
    QFile file(checkedPath(path));
    require(file.open(QIODevice::ReadOnly) && file.size() > 0, QStringLiteral("无法读取录音 %1").arg(path));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        checkCancel(cancel);
        const auto block = file.read(1024 * 1024);
        require(!block.isEmpty(), QStringLiteral("读取录音失败 %1").arg(path));
        hash.addData(block);
    }
    return hash.result();
}

void copyRecording(const QString &source, const QString &target, bool link, const std::atomic_bool &cancel,
                   const QByteArray &expectedHash = {})
{
    checkCancel(cancel);
    const auto sourcePath = checkedPath(source);
    const auto targetPath = checkedPath(target);
    if (link && CreateHardLinkW(reinterpret_cast<LPCWSTR>(targetPath.utf16()),
                                reinterpret_cast<LPCWSTR>(sourcePath.utf16()), nullptr))
        return;
    QFile input(sourcePath);
    QSaveFile output(targetPath);
    require(input.open(QIODevice::ReadOnly) && output.open(QIODevice::WriteOnly),
        QStringLiteral("无法复制录音 %1").arg(source));
    qint64 bytes = 0;
    const auto expected = input.size();
    QCryptographicHash digest(QCryptographicHash::Sha256);
    while (!input.atEnd()) {
        checkCancel(cancel);
        const auto block = input.read(1024 * 1024);
        require(!block.isEmpty() && output.write(block) == block.size(),
            QStringLiteral("复制录音失败 %1").arg(source));
        bytes += block.size();
        if (!expectedHash.isEmpty())
            digest.addData(block);
    }
    require(expectedHash.isEmpty() || digest.result() == expectedHash,
        QStringLiteral("录音在导入预览后发生变化，请重新导入 %1").arg(source));
    require(bytes == expected && output.commit(), QStringLiteral("无法保存录音 %1").arg(target));
}

}

RecordingPlan planRecordings(const QStringList &sources, const QString &current, const QJsonObject &groups,
    const QJsonObject &originalNames, const QString &replace, const QString &batch, const std::atomic_bool &cancel)
{
    RecordingPlan plan;
    try {
        QSet<QString> names;
        QSet<QByteArray> contents;
        if (!current.isEmpty()) {
            const QDir directory(checkedPath(current));
            require(directory.exists(), QStringLiteral("无法读取原录音目录 %1").arg(current));
            for (const auto &file : directory.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Name)) {
                const auto name = file.fileName();
                const auto group = groups.value(name).toString("legacy");
                if (group == replace) {
                    plan.replaced.append(name);
                    continue;
                }
                names.insert(name.toCaseFolded());
                contents.insert(sha256(file.absoluteFilePath(), cancel));
                plan.files.append({file.absoluteFilePath(), name, originalNames.value(name).toString(name), group, {}, false});
            }
        }
        for (const auto &path : sources) {
            const QFileInfo file(checkedPath(path));
            const auto original = file.fileName();
            const auto digest = sha256(path, cancel);
            if (contents.contains(digest)) {
                plan.duplicates.append(original);
                continue;
            }
            auto name = original;
            for (int suffix = 2; names.contains(name.toCaseFolded()); ++suffix)
                name = QStringLiteral("%1-%2.%3").arg(file.completeBaseName()).arg(suffix).arg(file.suffix());
            if (name != original)
                plan.conflicts.append(QStringLiteral("%1 → %2").arg(original, name));
            contents.insert(digest);
            names.insert(name.toCaseFolded());
            plan.files.append({path, name, original, batch, digest, true});
            ++plan.additions;
        }
    } catch (const std::exception &error) {
        plan.error = QString::fromUtf8(error.what());
    }
    return plan;
}

QString copyRecordings(const RecordingPlan &plan, const QString &directory, const QString &pendingDirectory,
    const QStringList &pending, const std::atomic_bool &cancel, std::atomic_int &completed)
{
    try {
        require(QDir().mkpath(checkedPath(directory)), QStringLiteral("无法创建录音目录 %1").arg(directory));
        if (!pendingDirectory.isEmpty())
            require(QDir().mkpath(checkedPath(pendingDirectory)), QStringLiteral("无法创建待切片目录"));
        for (const auto &file : plan.files) {
            const auto target = QDir(directory).filePath(file.name);
            copyRecording(file.source, target, !file.incoming, cancel, file.digest);
            if (!pendingDirectory.isEmpty() && (file.incoming || pending.contains(file.name)))
                copyRecording(target, QDir(pendingDirectory).filePath(file.name), true, cancel);
            ++completed;
        }
        checkCancel(cancel);
        return {};
    } catch (const std::exception &error) {
        return QString::fromUtf8(error.what());
    }
}

}
