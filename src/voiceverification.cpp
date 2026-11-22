#include "voiceverification.h"
#include "engine.h"

#include <windows.h>

#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <stdexcept>

namespace ez {
namespace {

QJsonObject fileVersion(const QString &path)
{
    const auto absolute = checkedPath(path);
    const auto file = CreateFileW(reinterpret_cast<LPCWSTR>(absolute.utf16()), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO timestamps{};
    const bool readable = GetFileInformationByHandle(file, &info)
        && GetFileInformationByHandleEx(file, FileBasicInfo, &timestamps, sizeof(timestamps));
    CloseHandle(file);
    if (!readable || (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
        return {};
    const auto wide = [](DWORD high, DWORD low) { return (quint64(high) << 32) | low; };
    return {{"volume", QString::number(info.dwVolumeSerialNumber)},
        {"file", QString::number(wide(info.nFileIndexHigh, info.nFileIndexLow))},
        {"bytes", QString::number(wide(info.nFileSizeHigh, info.nFileSizeLow))},
        {"modified", QString::number(timestamps.LastWriteTime.QuadPart)},
        {"changed", QString::number(timestamps.ChangeTime.QuadPart)}};
}

const QString verifiedJobs = QStringLiteral(
    " FROM jobs WHERE operation='synthesize' AND state='succeeded'"
    " AND json_type(CAST(result AS TEXT),'$.voice_verification')='object'");

}

bool sameSynthesisInputs(const QJsonObject &before, const QJsonObject &after)
{
    const auto comparable = [](QJsonObject version) {
        auto files = version.value("files").toObject();
        // Staging or removing a hard link changes the weight file's metadata timestamp.
        for (const auto *key : {"gpt", "sovits"}) {
            auto file = files.value(key).toObject();
            file.remove("changed");
            files[key] = file;
        }
        version["files"] = files;
        return version;
    };
    return !before.isEmpty() && !after.isEmpty() && comparable(before) == comparable(after);
}

QJsonObject voiceInputVersion(const QJsonObject &settings, const QString &launcher)
{
    if (launcher.isEmpty())
        return {};
    try {
        const auto engine = QFileInfo(checkedPath(launcher)).absolutePath();
        const QPair<QString, QString> sources[] = {
            {"gpt", settings.value("gpt").toString()}, {"sovits", settings.value("sovits").toString()},
            {"reference", settings.value("reference").toString()}, {"launcher", launcher},
            {"main", engine + "/webui.py"}, {"config", engine + "/config.py"},
            {"inference", engine + "/GPT_SoVITS/inference_webui_fast.py"},
        };
        QJsonObject files;
        for (const auto &[key, path] : sources) {
            if (path.isEmpty())
                return {};
            auto version = fileVersion(path);
            if (version.isEmpty())
                return {};
            version["path"] = path;
            files.insert(key, version);
        }
        const QJsonObject reference{{"family", settings.value("family")}, {"prompt", settings.value("prompt")},
            {"prompt_language", settings.value("prompt_language")}};
        const auto digest = QCryptographicHash::hash(QJsonDocument(reference).toJson(QJsonDocument::Compact),
                                                    QCryptographicHash::Sha256);
        return {{"files", files}, {"reference_settings", QString::fromLatin1(digest.toHex())}};
    } catch (const std::exception &) {
        return {};
    }
}

QJsonObject latestBatchVerification(const QString &voice, const QString &batch)
{
    if (voice.isEmpty() || batch.isEmpty())
        return {};
    QSqlQuery query;
    query.prepare("SELECT json_extract(CAST(result AS TEXT),'$.voice_verification')" + verifiedJobs
        + " AND json_extract(CAST(request AS TEXT),'$.voice_id')=?"
          " AND json_extract(CAST(request AS TEXT),'$.batch_id')=? ORDER BY rowid DESC LIMIT 1");
    query.addBindValue(voice);
    query.addBindValue(batch);
    if (!query.exec() || !query.next())
        return {};
    return QJsonDocument::fromJson(query.value(0).toByteArray()).object();
}

QSqlQuery voicesWithVerification(const QSqlDatabase &database)
{
    return QSqlQuery("SELECT voice.id,voice.name,voice.settings,voice.verified_engine,"
        " (SELECT json_extract(CAST(result AS TEXT),'$.voice_verification')" + verifiedJobs
        + " AND json_extract(CAST(request AS TEXT),'$.voice_id')=voice.id ORDER BY rowid DESC LIMIT 1)"
          " FROM voices AS voice ORDER BY voice.name", database);
}

QString voiceVerificationStatus(const QJsonObject &settings, const QJsonObject &record,
                                const QString &previousEngine, const QString &selectedEngine)
{
    if (record.isEmpty())
        return previousEngine.isEmpty() ? QStringLiteral("待验证") : QStringLiteral("待重新验证");
    const auto launcher = record.value("launcher").toString();
    if (!selectedEngine.isEmpty() && selectedEngine.compare(launcher, Qt::CaseInsensitive) != 0)
        return QStringLiteral("当前引擎待验证");
    const auto live = voiceInputVersion(settings, launcher);
    return !live.isEmpty() && live == record.value("input_version").toObject()
        ? QStringLiteral("已验证") : QStringLiteral("待重新验证");
}

}
