#include "compatdraft.h"
#include "compatibility.h"
#include "resources.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSysInfo>
#include <QTemporaryDir>
#include <limits>
#include <stdexcept>

namespace ez::adaptation {
namespace {

[[noreturn]] void fail(const QString &message)
{
    throw std::runtime_error(message.toStdString());
}

const QString inferenceScript = QStringLiteral("GPT_SoVITS/inference_webui_fast.py");

QJsonObject scripts(const QString &directory, const QStringList &paths)
{
    QJsonObject result;
    for (const auto &path : paths) {
        const auto digest = engineScriptDigest(directory, path);
        if (!digest.isEmpty())
            result[path] = digest;
    }
    return result;
}

QString verifyOperation(const Capture &capture, const QJsonObject &contract, const Check &check)
{
    const auto operation = check.operation;
    const auto base = operation.startsWith("resume_") ? QStringLiteral("train_") + operation.mid(7) : operation;
    const auto resource = resourceIssue(capture.directory, base, check.family, check.language);
    if (!resource.isEmpty())
        return resource;
    QJsonObject methods;
    const auto descriptors = contract.value("main").toObject();
    for (const auto &item : contract.value("operations").toObject().value(base).toArray()) {
        const auto method = item.toString();
        methods.insert(method, descriptors.value(method));
    }
    auto issues = compareEngineInterface(capture.main, methods);
    if (operation == "synthesize") {
        if (capture.inference.config.isEmpty())
            issues << capture.inferenceIssue;
        else
            issues << compareEngineInterface(capture.inference, contract.value("inference").toObject());
    }
    if (operation != base) {
        const auto resume = resumeEngineIssue(capture.directory, check.family, base, capture.adapter);
        if (!resume.isEmpty())
            issues << resume;
    }
    return issues.join('\n');
}

void write(const QDir &folder, const QString &name, const QByteArray &data)
{
    QSaveFile file(folder.filePath(name));
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        fail(QStringLiteral("无法写入 %1：%2").arg(name, file.errorString()));
}

QByteArray json(const QJsonObject &value) { return QJsonDocument(value).toJson(QJsonDocument::Indented); }

bool sameIdentity(const QJsonObject &a, const QJsonObject &b)
{
    return a.value("webui.py").isString() && a.value("webui.py") == b.value("webui.py")
        && a.value(inferenceScript).isString() && a.value(inferenceScript) == b.value(inferenceScript);
}

QString valueText(const QString &value)
{
    auto text = value.simplified();
    return text.replace('&', QStringLiteral("&amp;")).replace('<', QStringLiteral("&lt;")).replace('>', QStringLiteral("&gt;"));
}

}

QString Check::anchor() const
{
    auto result = QStringLiteral("check-") + operation;
    if (!family.isEmpty())
        result += '-' + family.toLower();
    if (!language.isEmpty())
        result += '-' + language;
    return result;
}

QString Check::label() const
{
    const auto option = family.isEmpty() ? (language == "zh" ? QStringLiteral("普通话")
        : language == "yue" ? QStringLiteral("粤语") : QString()) : family;
    return option.isEmpty() ? operationLabel(operation) : operationLabel(operation) + " · " + option;
}

Capture scan(const QString &path, const QString &adapter, bool captureInference)
{
    Capture result;
    result.directory = engineRoot(QDir(path).filePath("go-webui.bat"));
    result.adapter = adapter;
    const auto contract = engineContract(adapter);
    result.ports = enginePorts(result.directory, adapter);
    const auto startup = resourceIssue(result.directory, "start", {});
    if (!startup.isEmpty())
        fail(startup);
    result.fingerprint = engineFingerprint(result.directory);
    result.identity = scripts(result.directory, {"go-webui.bat", "webui.py", inferenceScript});
    if (!result.identity.contains("webui.py") || !result.identity.contains(inferenceScript))
        fail(QStringLiteral("找不到 webui.py 或 GPT_SoVITS/inference_webui_fast.py。请选择完整的整合包。"));
    result.main = capturePackageInterface(result.directory, result.ports.main);
    result.inferenceIssue = QStringLiteral("尚未采集快速推理。请在整合包 WebUI 启动快速推理后重新采集。");
    if (captureInference) {
        try {
            result.inference = capturePackageInterface(result.directory, result.ports.inference);
            result.inferenceIssue.clear();
        } catch (const std::exception &error) {
            result.inferenceIssue = QString::fromUtf8(error.what());
        }
    }
    for (const auto &operation : adapterOperations(adapter)) {
        const bool package = operation == "start" || operation == "slice" || operation == "asr";
        const auto options = operation == "asr" ? QStringList{"zh", "yue"} : package ? QStringList{QString()}
            : operation.startsWith("resume_") ? QStringList{"v5turbo"} : adapterFamilies(adapter);
        for (const auto &option : options) {
            Check check{operation, package ? QString() : option, operation == "asr" ? option : QString(), {}, {}};
            const auto base = operation.startsWith("resume_") ? QStringLiteral("train_") + operation.mid(7) : operation;
            check.issue = verifyOperation(result, contract, check);
            check.scripts = scripts(result.directory, resourceScripts(base, check.family, check.language));
            result.checks.append(std::move(check));
        }
    }
    if (engineFingerprint(result.directory) != result.fingerprint)
        fail(QStringLiteral("采集期间整合包脚本发生变化，请重新采集。"));
    return result;
}

QJsonObject loadBaseline(const QString &path)
{
    QFile file(path.isEmpty() ? QStringLiteral(":/compat/registry-v1.json") : checkedPath(path));
    if (!file.open(QIODevice::ReadOnly))
        fail(QStringLiteral("无法读取基准清单：%1").arg(file.errorString()));
    return validatedCompatibilityRegistry(file.read(1024 * 1024 + 1));
}

void validateDraft(const Capture &capture, const Draft &draft, bool localTest)
{
    static const QRegularExpression idPattern(R"(^[a-zA-Z0-9][a-zA-Z0-9._-]{0,79}$)");
    if (!idPattern.match(draft.id).hasMatch() || draft.name.trimmed().isEmpty() || draft.source.trimmed().isEmpty())
        fail(QStringLiteral("请填写整合包名称和来源。包 ID 使用英文字母、数字、点、下划线或短横线。"));
    if (draft.entries.isEmpty())
        fail(QStringLiteral("请在结果列表纳入至少一项操作。"));
    bool canTest = false;
    for (const auto &entry : draft.entries) {
        if (entry.checkIndex < 0 || entry.checkIndex >= capture.checks.size())
            fail(QStringLiteral("验收项目已变化，请重新采集。"));
        const auto &check = capture.checks.at(entry.checkIndex);
        if (!localTest && entry.result != "unverified" && entry.evidence.trimmed().isEmpty())
            fail(QStringLiteral("请为「%1」填写真实验收记录或失败现象。").arg(check.label()));
        if (entry.result == "passed" && !check.issue.isEmpty())
            fail(QStringLiteral("「%1」仍有核对问题，请解决后重新采集。").arg(check.label()));
        canTest |= entry.result != "failed" && check.issue.isEmpty();
    }
    if (localTest && !canTest)
        fail(QStringLiteral("目前没有通过接口和资源核对、可供本机测试的操作。"));
}

QString exportDraft(const Capture &capture, const Draft &draft, const QString &directory, bool localTest)
{
    validateDraft(capture, draft, localTest);
    const auto parent = checkedPath(directory);
    if (parent.compare(capture.directory, Qt::CaseInsensitive) == 0
        || parent.startsWith(capture.directory + '/', Qt::CaseInsensitive))
        fail(QStringLiteral("请选择整合包以外的输出目录。"));
    if (engineFingerprint(capture.directory) != capture.fingerprint)
        fail(QStringLiteral("整合包文件已变化，请回到上一步重新采集。"));
    auto registry = draft.baseline;
    const auto revision = registry.value("revision").toInt();
    if (revision < 1 || revision == std::numeric_limits<int>::max())
        fail(QStringLiteral("基准修订号无效。"));
    registry["revision"] = revision + 1;
    auto packages = registry.value("packages").toArray();
    int existing = -1;
    QJsonObject package{{"id", draft.id}, {"name", draft.name}, {"identity_source", draft.source},
                        {"adapter", capture.adapter}, {"scripts", capture.identity}};
    for (int index = 0; index < packages.size(); ++index) {
        const auto old = packages.at(index).toObject();
        const auto same = sameIdentity(old.value("scripts").toObject(), capture.identity);
        if (same && old.value("id") != draft.id)
            fail(QStringLiteral("这两个身份脚本已经登记在 %1。请使用已有 ID 更新该记录。").arg(old.value("id").toString()));
        if (old.value("id") != draft.id)
            continue;
        if (!same || old.value("adapter") != capture.adapter)
            fail(QStringLiteral("此 ID 已登记为其他构建或适配器，请使用新的包 ID。"));
        existing = index;
        if (old.contains("upstream_version"))
            package["upstream_version"] = old.value("upstream_version");
        package["capabilities"] = old.value("capabilities");
    }
    auto capabilities = package.value("capabilities").toArray();
    const auto reportName = existing < 0 ? QStringLiteral("acceptance-%1.md").arg(draft.id)
        : QStringLiteral("acceptance-%1-r%2.md").arg(draft.id).arg(revision + 1);
    QString report = QStringLiteral("# %1 适配验收\n\n- 包 ID：`%2`\n- 来源：%3\n- 适配器：`%4`\n"
        "- EZ 版本：`%5`\n- 系统：%6\n- 清单修订：%7 → %8\n\n")
        .arg(valueText(draft.name), draft.id, valueText(draft.source), capture.adapter,
             QStringLiteral(EZ_APP_VERSION), QSysInfo::prettyProductName()).arg(revision).arg(revision + 1);
    if (localTest)
        report += QStringLiteral("> 隔离验收用。临时通过项仅用于允许在独立数据目录中进行真实测试。\n\n");
    report += QStringLiteral("## 脚本摘要\n\n");
    for (auto item = capture.identity.constBegin(); item != capture.identity.constEnd(); ++item)
        report += QStringLiteral("- `%1` · `%2`\n").arg(item.key(), item.value().toString());
    QStringList summary;
    for (const auto &entry : draft.entries) {
        const auto &check = capture.checks.at(entry.checkIndex);
        const auto evidence = entry.evidence.trimmed();
        if (scripts(capture.directory, check.scripts.keys()) != check.scripts)
            fail(QStringLiteral("「%1」的脚本已变化，请重新采集。").arg(check.label()));
        const bool provisional = localTest && entry.result != "failed" && check.issue.isEmpty();
        const auto result = provisional ? QStringLiteral("passed") : entry.result;
        QJsonObject row{{"operation", check.operation}, {"resources", "20261006"}, {"result", result},
                        {"report", "compat/" + reportName + '#' + check.anchor()}};
        if (!check.family.isEmpty())
            row["family"] = check.family;
        if (!check.language.isEmpty())
            row["language"] = check.language;
        if (!check.scripts.isEmpty())
            row["scripts"] = check.scripts;
        if (result == "failed" && !evidence.isEmpty())
            row["reason"] = evidence;
        int index = 0;
        for (; index < capabilities.size(); ++index) {
            const auto previous = capabilities.at(index).toObject();
            if (previous.value("operation").toString() == check.operation
                && previous.value("family").toString() == check.family
                && previous.value("language").toString() == check.language)
                break;
        }
        const bool retainPublished = !localTest && entry.result == "unverified" && index < capabilities.size()
            && capabilities.at(index).toObject().value("result") != "unverified";
        if (index == capabilities.size())
            capabilities.append(row);
        else if (!retainPublished)
            capabilities.replace(index, row);
        const auto label = retainPublished ? QStringLiteral("沿用已登记结果，本次待复核")
            : provisional ? QStringLiteral("仅本机临时开放") : result == "passed" ? QStringLiteral("通过")
            : result == "failed" ? QStringLiteral("未通过") : QStringLiteral("待验收");
        summary << QStringLiteral("- %1 · %2").arg(check.label(), label);
        report += QStringLiteral("\n### %1\n\n%2 · %3\n\n").arg(check.anchor(), check.label(), label);
        if (!check.issue.isEmpty())
            report += QStringLiteral("接口或资源问题 · %1\n\n").arg(valueText(check.issue));
        report += evidence.isEmpty() ? QStringLiteral("验收记录 · 待填写\n") : QStringLiteral("验收记录 · %1\n").arg(valueText(evidence));
    }
    package["capabilities"] = capabilities;
    if (existing < 0)
        packages.append(package);
    else
        packages.replace(existing, package);
    registry["packages"] = packages;
    const auto candidate = json(registry);
    validatedCompatibilityRegistry(candidate);

    QDir output(parent);
    QTemporaryDir temporary(output.filePath(".ez-adaptation-XXXXXX"));
    if (!temporary.isValid())
        fail(QStringLiteral("无法在目标目录创建导出文件夹。"));
    const QDir staging(temporary.path());
    if (!staging.mkdir("capture-local"))
        fail(QStringLiteral("无法创建本地采集目录。"));
    const QDir local(staging.filePath("capture-local"));
    write(local, "main-config.json", json(capture.main.config));
    write(local, "main-info.json", json(capture.main.info));
    if (!capture.inference.config.isEmpty()) {
        write(local, "inference-config.json", json(capture.inference.config));
        write(local, "inference-info.json", json(capture.inference.info));
    }
    write(local, "capture.json", json({{"captured_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"adapter", capture.adapter}, {"scripts", capture.identity}, {"fingerprint", capture.fingerprint},
        {"main_port", int(capture.ports.main)}, {"inference_port", int(capture.ports.inference)},
        {"inference_issue", capture.inferenceIssue}}));
    write(staging, localTest ? "registry-local-test.json" : "registry-candidate.json", candidate);
    write(staging, reportName, report.toUtf8());
    if (!localTest) {
        const auto pr = QStringLiteral("# 适配 %1\n\n## 范围\n\n%2\n\n## 验收证据\n\n"
            "- 将候选清单审阅后合入 `compat/registry-v1.json`。\n"
            "- 将验收报告放在 `compat/%3`。\n"
            "- 提交前核对上游修订号，检查旧整合包回归。\n"
            "- `capture-local` 留在本地。\n").arg(valueText(draft.name), summary.join('\n'), reportName);
        write(staging, QStringLiteral("PR-草稿.md"), pr.toUtf8());
    }
    const auto title = QStringLiteral("%1-%2-%3").arg(localTest ? QStringLiteral("local-test") : QStringLiteral("compat-draft"),
        draft.id, QDateTime::currentDateTime().toString("yyyyMMddHHmmss"));
    auto target = title;
    for (int index = 2; output.exists(target); ++index)
        target = title + '-' + QString::number(index);
    if (!output.rename(QFileInfo(staging.path()).fileName(), target))
        fail(QStringLiteral("导出完成时无法保存目标文件夹，请重试。"));
    temporary.setAutoRemove(false);
    return output.filePath(target);
}

}
