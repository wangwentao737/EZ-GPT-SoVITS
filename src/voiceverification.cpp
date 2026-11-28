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

}
