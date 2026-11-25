#include "resources.h"
#include "compatibility.h"
#include "engine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QStorageInfo>
#include <stdexcept>

namespace ez {
namespace {

const QString models = "GPT_SoVITS/pretrained_models/";
const QString bert = models + "chinese-roberta-wwm-ext-large/";
const QString hubert = models + "chinese-hubert-base/";
const QMap<QString, QString> sovitsBases{
    {"v2", "gsv-v2final-pretrained/s2G2333k.pth"},
    {"v2Pro", "v2Pro/s2Gv2Pro.pth"}, {"v2ProPlus", "v2Pro/s2Gv2ProPlus.pth"},
    {"v5dev", "gsv-v5-pretrained/s2Gv5dev.pth"}, {"v5turbo", "gsv-v5-pretrained/s2Gv5turbo.pth"},
};

QStringList asrResources(const QString &language)
{
    const QStringList folders = language == "yue"
        ? QStringList{"speech_UniASR_asr_2pass-cantonese-CHS-16k-common-vocab1468-tensorflow1-online"}
        : QStringList{"speech_paraformer-large_asr_nat-zh-cn-16k-common-vocab8404-pytorch",
                      "speech_fsmn_vad_zh-cn-16k-common-pytorch",
                      "punc_ct-transformer_zh-cn-common-vocab272727-pytorch"};
    QStringList resources;
    for (const auto &folder : folders) {
        const auto path = "tools/asr/models/" + folder;
        resources << path + "/config.yaml" << path + "/model.pt";
    }
    return resources;
}

QStringList resourcesFor(const QString &operation, const QString &family, const QString &language)
{
    if (operation == "start")
        return {"go-webui.bat", "runtime/python.exe", "webui.py", "config.py"};
    if (operation == "slice")
        return {"tools/slice_audio.py"};
    if (operation == "asr")
        return asrResources(language);

    const auto v5 = family.startsWith("v5");
    const auto pro = family.startsWith("v2Pro");
    const auto base = models + sovitsBases.value(family);
    const auto sovitsConfig = pro ? "GPT_SoVITS/configs/s2" + family + ".json" : QStringLiteral("GPT_SoVITS/configs/s2.json");
    const auto speakerModel = models + "sv/pretrained_eres2netv2w24s4ep4.ckpt";
    const QStringList encoders{bert + "config.json", bert + "pytorch_model.bin",
        bert + "tokenizer.json", hubert + "config.json", hubert + "pytorch_model.bin"};
    if (operation == "synthesize") {
        auto resources = encoders;
        resources << "GPT_SoVITS/inference_webui_fast.py";
        if (v5)
            resources << models + "gsv-v5-pretrained/vocoder.pth";
        if (pro)
            resources << speakerModel;
        return resources;
    }
    if (operation == "prepare") {
        auto resources = encoders;
        resources << base << sovitsConfig << "GPT_SoVITS/prepare_datasets/1-get-text.py"
                  << "GPT_SoVITS/prepare_datasets/2-get-hubert-wav32k.py"
                  << "GPT_SoVITS/prepare_datasets/3-get-semantic.py";
        if (pro)
            resources << "GPT_SoVITS/prepare_datasets/2-get-sv.py" << speakerModel;
        return resources;
    }
    if (operation == "train_sovits") {
        if (v5)
            return {base, sovitsConfig, "GPT_SoVITS/s2_train_v3_lora.py"};
        auto discriminator = base;
        discriminator.replace("/s2G", "/s2D");
        return {base, discriminator, sovitsConfig, "GPT_SoVITS/s2_train.py"};
    }
    const auto gpt = family == "v2"
        ? "gsv-v2final-pretrained/s1bert25hz-5kh-longer-epoch=12-step=369668.ckpt" : "s1v3.ckpt";
    return {models + gpt, "GPT_SoVITS/configs/s1longer-v2.yaml", "GPT_SoVITS/s1_train.py"};
}

bool readableFile(const QString &path)
{
    QFile file(checkedPath(path));
    return QFileInfo(file).isFile() && file.open(QIODevice::ReadOnly) && file.size() > 0;
}

bool checkpointHeader(QFile &file, bool sovits)
{
    const auto header = file.read(4);
    if (!sovits && header.startsWith('\x80') && file.size() > 4)
        return true; // Legacy GPT checkpoints can use the binary pickle format.
    const bool wrapped = sovits && header.size() == 4 && header[0] == '0'
        && header[1] >= '0' && header[1] <= '8' && header.endsWith("\x03\x04");
    if (header != "PK\x03\x04" && !wrapped)
        return false;
    // PyTorch ZIP checkpoints have a central-directory footer, even with ZIP64.
    // Checking it avoids loading truncated files and does not deserialize weights.
    return file.seek(qMax<qint64>(0, file.size() - 65557)) && file.readAll().contains("PK\x05\x06");
}

ResourceCheck checkDisk(const QString &id, const QString &label, const QString &requested)
{
    const auto path = checkedPath(requested);
    QFileInfo directory(path);
    while (!directory.exists() && directory.absolutePath() != directory.absoluteFilePath())
        directory.setFile(directory.absolutePath());
    const QStorageInfo disk(directory.absoluteFilePath());
    if (!disk.isReady() || !disk.isValid())
        return {id, label, QStringLiteral("无法读取磁盘状态。请检查目录或重新连接磁盘。\n%1").arg(path), false};
    if (!directory.isDir() || !directory.isWritable() || disk.isReadOnly())
        return {id, label, QStringLiteral("目录不可写。请在「设置」中选择可写位置或检查权限。\n%1").arg(path), false};
    if (disk.bytesAvailable() <= 0)
        return {id, label, QStringLiteral("磁盘空间不足，请释放空间后复查。\n%1").arg(path), false};
    return {id, label, QStringLiteral("可用空间 %1 GiB\n%2").arg(disk.bytesAvailable() / 1073741824.0, 0, 'f', 1).arg(path), true};
}

}

QStringList engineOperations()
{
    return {"synthesize", "slice", "asr", "prepare", "train_sovits", "train_gpt"};
}

QString operationLabel(const QString &operation)
{
    static const QMap<QString, QString> labels{
        {"start", QStringLiteral("引擎启动")}, {"synthesize", QStringLiteral("语音合成")},
        {"slice", QStringLiteral("切片")}, {"asr", QStringLiteral("自动转写")},
        {"prepare", QStringLiteral("准备训练数据")}, {"train_sovits", QStringLiteral("SoVITS 训练")},
        {"train_gpt", QStringLiteral("GPT 训练")},
        {"resume_sovits", QStringLiteral("SoVITS 续训")}, {"resume_gpt", QStringLiteral("GPT 续训")},
    };
    return labels.value(operation, operation);
}

bool resourceRuleIncludesScript(const QString &operation, const QString &family,
                                const QString &language, const QString &script)
{
    return script.endsWith(".py") && resourcesFor(operation, family, language).contains(script);
}

QString resourceIssue(const QString &engine, const QString &operation, const QString &family,
                      const QString &asrLanguage, const std::atomic_bool *cancelled)
{
    if (operation != "start" && !engineOperations().contains(operation))
        return QStringLiteral("当前适配暂不支持此操作：%1").arg(operation);
    if (operation != "start" && operation != "slice" && operation != "asr" && !sovitsBases.contains(family))
        return QStringLiteral("当前适配暂不支持此模型家族：%1").arg(family);
    if (operation == "asr" && asrLanguage != "zh" && asrLanguage != "yue")
        return QStringLiteral("本地转写支持普通话和粤语。其他语种可导入标注或手工录入。");

    QStringList missing;
    for (const auto &resource : resourcesFor(operation, family, asrLanguage)) {
        if (cancelled && cancelled->load())
            return {};
        if (!readableFile(engine + '/' + resource))
            missing << resource;
    }
    if (missing.isEmpty())
        return {};
    const auto action = operation == "asr" ? QStringLiteral("可导入标注或手工录入，补齐模型后可重试转写。")
                                           : QStringLiteral("请补齐资源或在「设置」中选择完整的 GPT-SoVITS 安装。");
    return QStringLiteral("%1缺少资源或文件不可读。%2\n%3")
        .arg(operationLabel(operation), action, missing.join('\n'));
}

QString jobResourceIssue(const QString &engine, const QString &data, const QJsonObject &request)
{
    const auto operation = request.value("operation").toString();
    const auto family = request.value("family").toString();
    const auto issue = resourceIssue(engine, operation, family, request.value("asr_language").toString("zh"));
    if (!issue.isEmpty() || operation != "synthesize")
        return issue;

    const QPair<QString, QString> files[] = {
        {"gpt", QStringLiteral("GPT 权重")}, {"sovits", QStringLiteral("SoVITS 权重")},
        {"reference", QStringLiteral("参考音频")},
    };
    const auto library = data.endsWith('/') ? data : data + '/';
    for (const auto &[field, label] : files) {
        const auto selected = request.value(field).toString();
        if (selected.isEmpty())
            return QStringLiteral("声音缺少%1，请重新导入声音。").arg(label);
        const auto path = checkedPath(selected);
        if (!path.startsWith(library, Qt::CaseInsensitive))
            return QStringLiteral("%1位于当前数据目录之外，请重新导入声音。").arg(label);
        QFile file(path);
        if (!QFileInfo(file).isFile() || !file.open(QIODevice::ReadOnly) || file.size() <= 0)
            return QStringLiteral("%1缺失或不可读，请重新导入声音。\n%2").arg(label, path);
        if (field != "reference" && !checkpointHeader(file, field == "sovits"))
            return QStringLiteral("%1文件格式无效或已截断，请更换完整的权重。\n%2").arg(label, path);
        if (field == "sovits" && file.seek(0)) {
            const auto tag = file.read(2);
            if (tag == "07" || tag == "08") {
                const auto required = tag == "07" ? "v5dev" : "v5turbo";
                if (required != family)
                    return QStringLiteral("SoVITS 权重属于 %1，请按实际模型家族重新导入声音。").arg(required);
                const auto base = models + sovitsBases.value(required);
                if (!readableFile(engine + '/' + base))
                    return QStringLiteral("此 SoVITS 权重需要训练底模，请补齐资源。\n%1").arg(base);
            }
        }
    }
    return {};
}

bool readableTrainingCheckpoint(const QString &path)
{
    QFile file(checkedPath(path));
    return QFileInfo(file).isFile() && file.open(QIODevice::ReadOnly)
        && file.peek(4) == "PK\x03\x04" && checkpointHeader(file, false);
}

QList<ResourceCheck> inspectResources(const QString &launcher, const QString &family, const QString &data,
                                      const QString &output, const QJsonObject &voice,
                                      const EngineInstallation &installation, const QString &only,
                                      const std::atomic_bool &cancelled)
{
    QList<ResourceCheck> checks;
    const auto run = [&](const QString &id, const QString &label, auto check) {
        if (cancelled.load() || (!only.isEmpty() && id != only))
            return;
        try {
            checks.append(check());
        } catch (const std::exception &error) {
            checks.append({id, label, QString::fromUtf8(error.what()), false});
        }
    };
    const auto disk = [&](const QString &id, const QString &label, const QString &path) {
        run(id, label, [&] { return checkDisk(id, label, path); });
    };

    const auto operations = compatibilityOperations() + QStringList{"asr-yue"};
    for (const auto &id : operations) {
        const auto operation = id == "asr-yue" ? QString("asr") : id;
        const auto label = id == "asr-yue" ? QStringLiteral("粤语自动转写")
            : id == "asr" ? QStringLiteral("普通话自动转写") : operationLabel(operation);
        run(id, label, [&] {
            const auto issue = installation.issue(operation, family, id == "asr-yue" ? "yue" : "zh");
            return ResourceCheck{id, label, issue.isEmpty() ? QStringLiteral("已登记，所需文件可读。") : issue, issue.isEmpty()};
        });
    }
    if (!voice.isEmpty()) {
        const auto label = QStringLiteral("声音预设 · %1").arg(voice.value("name").toString());
        run("voice", label, [&] {
            auto request = voice;
            request["operation"] = "synthesize";
            auto issue = installation.issue("synthesize", request.value("family").toString());
            if (issue.isEmpty())
                issue = jobResourceIssue(engineRoot(launcher), data, request);
            return ResourceCheck{"voice", label, issue.isEmpty()
                ? QStringLiteral("两份权重和参考音频可读，可提交试音验证。") : issue, issue.isEmpty()};
        });
    }
    run("engine-disk", QStringLiteral("引擎工作目录"), [&] {
        return checkDisk("engine-disk", QStringLiteral("引擎工作目录"), engineRoot(launcher));
    });
    disk("data-disk", QStringLiteral("项目和素材目录"), data);
    disk("output-disk", QStringLiteral("成品目录"), output);
    return cancelled.load() ? QList<ResourceCheck>{} : checks;
}

}
