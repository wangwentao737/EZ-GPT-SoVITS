#include "compatibility.h"
#include "engine.h"
#include "resources.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QVersionNumber>
#include <algorithm>
#include <stdexcept>

namespace ez {
namespace {

constexpr auto maxRegistryBytes = 1024 * 1024;
constexpr auto inferenceScript = "GPT_SoVITS/inference_webui_fast.py";
constexpr auto legacyAdapter = "0725-cu124";
const QStringList identityScripts{"go-webui.bat", "webui.py", inferenceScript};

[[noreturn]] void invalid(const QString &message)
{
    throw std::runtime_error(message.toStdString());
}

QString operationLanguage(const QString &operation, const QString &language)
{
    return operation == "asr" ? language : QString();
}

bool packageOperation(const QString &operation)
{
    return operation == "start" || operation == "slice" || operation == "asr";
}

QString baseOperation(const QString &operation)
{
    return operation.startsWith("resume_") ? "train_" + operation.mid(7) : operation;
}

bool adapterSupportsOperation(const QString &adapter, const QString &operation)
{
    return adapter != legacyAdapter || operation == "start" || operation == "synthesize";
}

QJsonObject capability(const QJsonObject &entry, const QString &operation, const QString &family, const QString &language)
{
    for (const auto &value : entry.value("capabilities").toArray()) {
        const auto row = value.toObject();
        if (row.value("operation") == operation
            && row.value("family").toString() == (packageOperation(operation) ? QString() : family)
            && row.value("language").toString() == operationLanguage(operation, language))
            return row;
    }
    return {};
}

QJsonObject readRegistry(const QByteArray &contents)
{
    if (contents.size() > maxRegistryBytes)
        invalid(QStringLiteral("兼容清单超过 1 MiB。"));
    const auto registry = QJsonDocument::fromJson(contents).object();
    if (registry.value("format").toInt() != 1 || registry.value("revision").toInt() < 1
        || !registry.value("packages").isArray())
        invalid(QStringLiteral("兼容清单格式或修订号无效。"));
    static const QRegularExpression version(R"(^[0-9]+\.[0-9]+\.[0-9]+$)");
    const auto minimum = registry.value("ez_min").toString();
    const auto maximum = registry.value("ez_max").toString();
    const auto app = QVersionNumber::fromString(QStringLiteral(EZ_APP_VERSION));
    if (!version.match(minimum).hasMatch() || !version.match(maximum).hasMatch()
        || app < QVersionNumber::fromString(minimum) || app > QVersionNumber::fromString(maximum))
        invalid(QStringLiteral("此清单的适用范围不包含当前 EZ 版本。"));
    static const QRegularExpression idPattern(R"(^[a-zA-Z0-9][a-zA-Z0-9._-]{0,79}$)");
    static const QRegularExpression sha256("^[a-f0-9]{64}$");
    QSet<QString> ids, signatures;
    for (const auto &value : registry.value("packages").toArray()) {
        const auto entry = value.toObject();
        const auto id = entry.value("id").toString();
        const auto adapter = entry.value("adapter").toString();
        const auto scripts = entry.value("scripts").toObject();
        const auto versionEvidence = entry.value("upstream_version").toObject();
        if (!idPattern.match(id).hasMatch() || ids.contains(id) || entry.value("name").toString().trimmed().isEmpty()
            || entry.value("identity_source").toString().trimmed().isEmpty()
            || !entry.value("capabilities").isArray() || scripts.size() != (scripts.contains("go-webui.bat") ? 3 : 2)
            || adapterContractFile(adapter).isEmpty()
            || (entry.contains("upstream_version") && (versionEvidence.value("value").toString().trimmed().isEmpty()
                || versionEvidence.value("source").toString().trimmed().isEmpty())))
            invalid(QStringLiteral("整合包条目无效、重名或引用了未知适配器：%1").arg(id));
        ids.insert(id);
        QString signature;
        for (const auto &path : identityScripts) {
            if (path == "go-webui.bat" && !scripts.contains(path))
                continue;
            const auto digest = scripts.value(path).toString();
            if (!sha256.match(digest).hasMatch())
                invalid(QStringLiteral("%1 缺少有效的脚本摘要：%2").arg(id, path));
            if (path != "go-webui.bat")
                signature += digest;
        }
        if (signatures.contains(signature))
            invalid(QStringLiteral("多个整合包条目使用相同的匹配依据：%1").arg(id));
        signatures.insert(signature);
        QSet<QString> combinations;
        for (const auto &item : entry.value("capabilities").toArray()) {
            const auto row = item.toObject();
            const auto operation = row.value("operation").toString();
            const auto family = row.value("family").toString();
            const auto language = row.value("language").toString();
            const auto result = row.value("result").toString();
            const auto key = operation + '/' + family + '/' + language;
            if (!compatibilityOperations().contains(operation) || combinations.contains(key)
                || !adapterSupportsOperation(adapter, operation)
                || (packageOperation(operation) ? row.contains("family") : !adapterFamilies(adapter).contains(family))
                || (operation == "asr" ? language != "zh" && language != "yue" : row.contains("language"))
                || (operation.startsWith("resume_") && family != "v5turbo")
                || row.value("resources") != "20261006"
                || (result != "passed" && result != "failed" && result != "unverified")
                || (row.contains("reason") && row.value("reason").toString().trimmed().isEmpty())
                || (result != "unverified" && row.value("report").toString().trimmed().isEmpty()))
                invalid(QStringLiteral("%1 的验证项无效或引用了客户端未实现的操作、家族或资源规则：%2").arg(id, key));
            const auto operationScripts = row.value("scripts").toObject();
            if (row.contains("scripts") && !row.value("scripts").isObject())
                invalid(QStringLiteral("%1 的脚本核对规则无效：%2").arg(id, key));
            for (auto script = operationScripts.constBegin(); script != operationScripts.constEnd(); ++script) {
                if (!resourceRuleIncludesScript(baseOperation(operation), family, language, script.key())
                    || !sha256.match(script.value().toString()).hasMatch())
                    invalid(QStringLiteral("%1 的操作脚本摘要无效：%2").arg(id, script.key()));
            }
            combinations.insert(key);
        }
    }
    return registry;
}

QPair<QString, QString> instanceKey(const QString &directory, const QString &fingerprint)
{
    return {directory.toCaseFolded(), fingerprint};
}

}

QString defaultEngineAdapter() { return "20261006"; }

QJsonObject validatedCompatibilityRegistry(const QByteArray &contents)
{
    return readRegistry(contents);
}

QStringList supportedEngineAdapters()
{
    return {defaultEngineAdapter(), QString::fromLatin1(legacyAdapter)};
}

QStringList adapterOperations(const QString &adapter)
{
    QStringList supported;
    for (const auto &operation : compatibilityOperations()) {
        if (adapterSupportsOperation(adapter, operation))
            supported.append(operation);
    }
    return supported;
}

QString adapterContractFile(const QString &adapter)
{
    if (adapter == defaultEngineAdapter() || adapter == legacyAdapter)
        return adapter + "-contract.json";
    return {};
}

QStringList adapterFamilies(const QString &adapter)
{
    if (adapter == legacyAdapter)
        return {"v2Pro", "v2ProPlus", "v2"};
    return {"v5turbo", "v5dev", "v2Pro", "v2ProPlus", "v2"};
}

QStringList compatibilityOperations()
{
    return QStringList{"start"} + engineOperations() + QStringList{"resume_sovits", "resume_gpt"};
}

QString compatibilityOperation(const QJsonObject &request)
{
    const auto operation = request.value("operation").toString();
    return (operation == "train_sovits" || operation == "train_gpt") && !request.value("resume_job").toString().isEmpty()
        ? "resume_" + operation.mid(6) : operation;
}

QString engineScriptDigest(const QString &directory, const QString &relativePath)
{
    QFile file(checkedPath(directory + '/' + relativePath));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? QString::fromLatin1(hash.result().toHex()) : QString();
}

QString EngineInstallation::package() const { return entry.value("id").toString(); }

QString EngineInstallation::operationFingerprint(const QString &operation, const QString &family, const QString &language) const
{
    auto fingerprint = operation == "synthesize" ? mainDigest + '/' + inferenceDigest : mainDigest;
    const auto scripts = capability(entry, operation, family, language).value("scripts").toObject();
    if (!scripts.isEmpty()) {
        QCryptographicHash digest(QCryptographicHash::Sha256);
        for (const auto &path : scripts.keys())
            digest.addData((path + '=' + scripts.value(path).toString() + '\n').toUtf8());
        fingerprint += '/' + QString::fromLatin1(digest.result().toHex());
    }
    return fingerprint;
}

QString EngineInstallation::description() const
{
    if (!error.isEmpty())
        return error;
    if (entry.isEmpty())
        return QStringLiteral("版本未确认 · 此整合包的脚本摘要尚未登记");
    QStringList details{QStringLiteral("%1 · %2").arg(entry.value("name").toString(), entry.value("identity_source").toString())};
    const auto version = entry.value("upstream_version").toObject();
    if (!version.isEmpty())
        details << QStringLiteral("上游声明 %1 · %2").arg(version.value("value").toString(), version.value("source").toString());
    if (!identityNote.isEmpty())
        details << identityNote;
    return details.join('\n');
}

QString EngineInstallation::issue(const QString &operation, const QString &family, const QString &language, bool trial) const
{
    if (!error.isEmpty())
        return error;
    try {
        const auto start = resourceIssue(directory, "start", {});
        if (!start.isEmpty())
            return start;
        if (!adapterSupportsOperation(adapter, operation))
            return QStringLiteral("%1 适配器尚未开放%2。").arg(adapter, operationLabel(operation));
        if (operation == "synthesize" && !adapterFamilies(adapter).contains(family))
            return QStringLiteral("%1 适配器尚未支持此模型家族：%2").arg(adapter, family);
        const auto shared = interfaces.value(baseOperation(operation));
        const auto runtime = operation == "synthesize" ? interfaces.value("synthesize_fast/" + family, shared) : shared;
        if (!runtime.isEmpty())
            return runtime;
        if (operation == "synthesize" && !entry.isEmpty()) {
            const auto expected = entry.value("scripts").toObject().value(inferenceScript).toString();
            if (!inferenceDigest.isEmpty() && inferenceDigest != expected)
                return QStringLiteral("合成接口已变化，脚本摘要与登记不符：%1").arg(inferenceScript);
        }
        const auto row = capability(entry, operation, family, language);
        if (row.value("result") == "failed")
            return QStringLiteral("%1\n%2").arg(row.value("reason").toString(QStringLiteral("EZ 验证未通过。")),
                                                row.value("report").toString());
        const auto local = resourceIssue(directory, baseOperation(operation), family, language);
        if (!local.isEmpty())
            return local;
        const auto scripts = row.value("scripts").toObject();
        for (auto script = scripts.constBegin(); script != scripts.constEnd(); ++script) {
            if (engineScriptDigest(directory, script.key()) != script.value().toString())
                return QStringLiteral("%1的脚本摘要与登记不符：%2").arg(operationLabel(operation), script.key());
        }
        if (operation.startsWith("resume_")) {
            const auto resume = resumeEngineIssue(directory, family, baseOperation(operation), adapter);
            if (!resume.isEmpty())
                return resume;
        }
        if (row.value("result") == "passed")
            return {};
        if (trial && operation == "synthesize" && entry.isEmpty() && connected)
            return {};
        const auto label = packageOperation(operation) ? operationLabel(operation) : family + " · " + operationLabel(operation);
        return QStringLiteral("%1尚未验证。%2").arg(label, entry.isEmpty() ? description() : QString());
    } catch (const std::exception &error) {
        return QString::fromUtf8(error.what());
    }
}

QStringList EngineInstallation::families(const QStringList &operations) const
{
    QStringList available;
    for (const auto &family : adapterFamilies(adapter)) {
        if (std::all_of(operations.cbegin(), operations.cend(), [&](const auto &operation) {
                return issue(operation, family).isEmpty();
            }))
            available.append(family);
    }
    return available;
}

QString EngineInstallation::report(const QString &operation, const QString &family) const
{
    return capability(entry, operation, family, "zh").value("report").toString();
}

CompatibilityRegistry::CompatibilityRegistry(const QString &data, EngineManager &engine)
    : QObject(nullptr), cache_(data + "/compat-registry-v1.json"), engine_(engine)
{
    QFile bundled(":/compat/registry-v1.json");
    if (!bundled.open(QIODevice::ReadOnly))
        invalid(QStringLiteral("缺少内置兼容清单。"));
    registry_ = readRegistry(bundled.read(maxRegistryBytes + 1));
    try {
        QFile cache(checkedPath(cache_));
        if (cache.open(QIODevice::ReadOnly)) {
            const auto saved = readRegistry(cache.read(maxRegistryBytes + 1));
            if (saved.value("revision").toInt() > revision())
                registry_ = saved;
        }
    } catch (const std::exception &) {
        // A cache from an older client or an interrupted import leaves the bundled registry available.
    }
    connect(&engine_, &EngineManager::readinessChanged, this, [this](bool ready) {
        if (ready) {
            auto &known = interfaceChecks_[instanceKey(engine_.root(), engine_.fingerprint())];
            auto observed = engine_.interfaceIssues();
            for (auto issue = known.cbegin(); issue != known.cend(); ++issue) {
                if (issue.key().startsWith("synthesize_fast/"))
                    observed.insert(issue.key(), issue.value());
            }
            known = std::move(observed);
        }
        emit changed();
    });
}

EngineInstallation CompatibilityRegistry::inspect(const QString &launcher) const
{
    EngineInstallation result;
    result.adapter = defaultEngineAdapter();
    if (launcher.isEmpty()) {
        result.error = QStringLiteral("请在「设置」中选择整合包。");
        return result;
    }
    try {
        result.directory = engineRoot(launcher);
        QJsonObject observed;
        for (const auto &path : identityScripts)
            observed[path] = engineScriptDigest(result.directory, path);
        result.mainDigest = observed.value("webui.py").toString();
        result.inferenceDigest = observed.value(inferenceScript).toString();
        result.fingerprint = engineFingerprint(result.directory);
        QList<QJsonObject> candidates;
        for (const auto &value : registry_.value("packages").toArray()) {
            const auto entry = value.toObject();
            const auto scripts = entry.value("scripts").toObject();
            if (scripts.value("webui.py") != observed.value("webui.py"))
                continue;
            if (scripts.value(inferenceScript) == observed.value(inferenceScript)) {
                candidates = {entry};
                break;
            }
            candidates.append(entry);
        }
        if (candidates.size() > 1)
            result.error = QStringLiteral("多个登记构建的主入口相同，推理脚本摘要无法消除匹配冲突。");
        if (candidates.size() == 1) {
            result.entry = candidates.constFirst();
            result.adapter = result.entry.value("adapter").toString();
            const auto referenceLauncher = result.entry.value("scripts").toObject().value("go-webui.bat").toString();
            if (!referenceLauncher.isEmpty() && referenceLauncher != observed.value("go-webui.bat").toString())
                result.identityNote = QStringLiteral("启动 BAT 与采集样本有差异，启动后按实际接口检查。");
        }
        result.interfaces = interfaceChecks_.value(instanceKey(result.directory, result.fingerprint));
        result.connected = engine_.ready() && engine_.root().compare(result.directory, Qt::CaseInsensitive) == 0
            && engine_.fingerprint() == result.fingerprint && engine_.adapter() == result.adapter;
    } catch (const std::exception &error) {
        result.error = QString::fromUtf8(error.what());
    }
    return result;
}

int CompatibilityRegistry::revision() const { return registry_.value("revision").toInt(); }

QString CompatibilityRegistry::install(const QByteArray &contents)
{
    const auto candidate = readRegistry(contents);
    const auto next = candidate.value("revision").toInt();
    if (next < revision())
        invalid(QStringLiteral("此清单修订 %1 早于当前修订 %2。").arg(next).arg(revision()));
    if (next == revision()) {
        if (candidate != registry_)
            invalid(QStringLiteral("同一修订号包含不同的清单内容。"));
        return QStringLiteral("兼容清单已是最新 · 修订 %1").arg(revision());
    }
    QSaveFile file(checkedPath(cache_));
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit())
        invalid(QStringLiteral("无法保存兼容清单：%1").arg(file.errorString()));
    registry_ = candidate;
    emit changed();
    return QStringLiteral("已更新兼容清单 · 修订 %1").arg(revision());
}

void CompatibilityRegistry::checkOnline()
{
    if (downloading_)
        return;
    if (!network_)
        network_ = new QNetworkAccessManager(this);
    QNetworkRequest request(QUrl("https://api.github.com/repos/wangwentao737/EZ-GPT-SoVITS/contents/compat/registry-v1.json?ref=main"));
    request.setRawHeader("Accept", "application/vnd.github.raw+json");
    request.setRawHeader("User-Agent", "EZ-GPT-SoVITS");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(10000);
    auto *reply = network_->get(request);
    downloading_ = true;
    QTimer::singleShot(12000, reply, [reply] { if (reply->isRunning()) reply->abort(); });
    connect(reply, &QNetworkReply::readyRead, this, [reply] {
        if (reply->bytesAvailable() > maxRegistryBytes)
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        downloading_ = false;
        reply->deleteLater();
        try {
            if (reply->error() != QNetworkReply::NoError
                || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
                invalid(QStringLiteral("无法从 GitHub 读取兼容清单：%1").arg(reply->errorString()));
            emit updateFinished(install(reply->read(maxRegistryBytes + 1)));
        } catch (const std::exception &error) {
            emit updateFinished(QStringLiteral("%1 继续使用修订 %2。").arg(QString::fromUtf8(error.what())).arg(revision()));
        }
    });
}

void CompatibilityRegistry::recordInferenceIssue(const QString &launcher, const QString &fingerprint,
                                                 const QString &family, const QString &issue)
{
    interfaceChecks_[instanceKey(engineRoot(launcher), fingerprint)]["synthesize_fast/" + family] = issue;
    emit changed();
}

void CompatibilityRegistry::clearInterfaceIssues(const QString &launcher)
{
    const auto installation = inspect(launcher);
    if (!installation.directory.isEmpty())
        interfaceChecks_.remove(instanceKey(installation.directory, installation.fingerprint));
    emit changed();
}

}
