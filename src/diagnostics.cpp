#include "diagnostics.h"
#include "engine.h"
#include "resources.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringDecoder>
#include <QSysInfo>
#include <QVBoxLayout>
#include <stdexcept>

namespace ez {
namespace {

QString errorSummary(const QString &message)
{
    const auto text = message.toLower();
    if (text.contains(QStringLiteral("显存不足")) || text.contains("out of memory"))
        return QStringLiteral("内存或显存不足。");
    if (text.contains(QStringLiteral("磁盘空间")) || text.contains("no space left")
        || text.contains("disk full"))
        return QStringLiteral("磁盘空间不足。");
    if (text.contains(QStringLiteral("端口")) || text.contains("address already in use"))
        return QStringLiteral("引擎端口不可用。");
    if (text.contains(QStringLiteral("不可写")) || text.contains("permission denied"))
        return QStringLiteral("文件或目录无法写入。");
    if (text.contains(QStringLiteral("预处理索引")) || text.contains(QStringLiteral("预处理样本")))
        return QStringLiteral("预处理产物缺失、不完整或与标注不匹配。");
    if (text.contains(QStringLiteral("缺少资源")) || text.contains(QStringLiteral("不可读"))
        || text.contains(QStringLiteral("文件格式无效")))
        return QStringLiteral("任务所需文件缺失、不可读或格式无效。");
    if (text.contains(QStringLiteral("接口")) || text.contains(QStringLiteral("就绪")))
        return QStringLiteral("引擎或推理服务未就绪。");
    return QStringLiteral("任务失败。可勾选原始错误详情和对应日志。");
}

QString logTail(const QString &path)
{
    QFile file(checkedPath(path));
    if (!file.open(QIODevice::ReadOnly))
        return QStringLiteral("无法读取日志。");
    constexpr qint64 limit = 64 * 1024;
    const auto offset = qMax<qint64>(0, file.size() - limit);
    const auto bom = file.peek(2);
    const bool utf16 = bom == "\xff\xfe" || bom == "\xfe\xff";
    if (!file.seek(utf16 ? offset / 2 * 2 : offset))
        return QStringLiteral("无法读取日志末尾。");
    auto bytes = file.read(limit);
    QString result;
    if (utf16) {
        QStringDecoder decoder(bom == "\xff\xfe" ? QStringDecoder::Utf16LE : QStringDecoder::Utf16BE);
        result = decoder(bytes);
    } else {
        bytes.replace(QByteArray(1, '\0'), QByteArray());
        QStringDecoder utf8(QStringDecoder::Utf8);
        result = utf8(bytes);
        if (utf8.hasError()) {
            QStringDecoder system(QStringDecoder::System);
            result = system(bytes);
        }
    }
    if (offset > 0)
        return QStringLiteral("仅显示最近的 64 KiB 日志。\n") + result;
    return result.isEmpty() ? QStringLiteral("日志为空。") : result;
}

}

bool exportDiagnostics(QWidget *parent, const QString &root, const QString &engineStatus,
                       const QString &resourceReport, const QString &jobId)
{
    QSqlQuery query;
    if (jobId.isEmpty()) {
        query.prepare("SELECT id, operation, state, message, created FROM jobs "
                      "ORDER BY CASE state WHEN 'running' THEN 0 WHEN 'queued' THEN 1 ELSE 2 END, rowid DESC LIMIT 1");
    } else {
        query.prepare("SELECT id, operation, state, message, created FROM jobs WHERE id=?");
        query.addBindValue(jobId);
    }
    if (!query.exec())
        throw std::runtime_error(query.lastError().text().toStdString());

    QString summary = QStringLiteral("EZ-GPT-SoVITS %1\nWindows · %2\n引擎状态 · %3\n\n资源检查\n%4")
        .arg(QCoreApplication::applicationVersion(), QSysInfo::prettyProductName(), engineStatus,
             resourceReport.isEmpty() ? QStringLiteral("本次运行尚未完成资源检查。") : resourceReport);
    QString task;
    QString detail;
    if (query.next()) {
        task = query.value(0).toString();
        const auto state = query.value(2).toString();
        detail = query.value(3).toString();
        const QMap<QString, QString> states{{"queued", QStringLiteral("排队")}, {"running", QStringLiteral("运行")},
            {"succeeded", QStringLiteral("成功")}, {"failed", QStringLiteral("失败")},
            {"cancelled", QStringLiteral("已取消")}, {"interrupted", QStringLiteral("中断")}};
        summary += QStringLiteral("\n\n任务\n编号 · %1\n阶段 · %2\n状态 · %3\n提交时间 · %4")
            .arg(task, operationLabel(query.value(1).toString()), states.value(state, state), query.value(4).toString());
        if (!detail.isEmpty() || state == "failed")
            summary += QStringLiteral("\n错误摘要 · %1").arg(errorSummary(detail));
    }
    query.finish();

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("诊断导出预览"));
    dialog.resize(670, 540);
    auto *layout = new QVBoxLayout(&dialog);
    auto *hint = new QLabel(QStringLiteral("原始日志可选，可能包含文件路径或台词。请检查预览再分享。"), &dialog);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *options = new QHBoxLayout;
    layout->addLayout(options);
    auto *preview = new QPlainTextEdit(&dialog);
    preview->setReadOnly(true);
    preview->setPlainText(summary);
    layout->addWidget(preview, 1);

    QList<QPair<QCheckBox *, QString>> sections;
    const auto rebuild = [&] {
        auto content = summary;
        for (const auto &[include, text] : sections) {
            if (include->isChecked())
                content += "\n\n" + include->text() + '\n' + text;
        }
        preview->setPlainText(content);
    };
    const auto addOption = [&](const QString &name, const QString &path, const QString &inlineText = {}) {
        bool available = !inlineText.isEmpty();
        if (!path.isEmpty()) {
            try {
                available = QFileInfo(checkedPath(path)).isFile();
            } catch (const std::exception &) {
                available = false;
            }
        }
        auto *box = new QCheckBox(name, &dialog);
        box->setEnabled(available);
        sections.append({box, inlineText});
        const auto index = sections.size() - 1;
        options->addWidget(box);
        QObject::connect(box, &QCheckBox::toggled, &dialog, [&, path, index](bool enabled) {
            if (enabled && !path.isEmpty()) {
                try {
                    sections[index].second = logTail(path);
                } catch (const std::exception &error) {
                    sections[index].second = QString::fromUtf8(error.what());
                }
            }
            rebuild();
        });
    };
    addOption(QStringLiteral("启动日志"), root + "/engine-start.log");
    const QRegularExpression identifier("^[0-9a-fA-F-]{36}$");
    if (identifier.match(task).hasMatch()) {
        addOption(QStringLiteral("任务错误日志"), root + "/jobs/" + task + "/error.log");
        addOption(QStringLiteral("任务输出日志"), root + "/jobs/" + task + "/result.log");
    }
    if (!detail.isEmpty())
        addOption(QStringLiteral("原始错误详情"), {}, detail);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存诊断"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    auto *copy = buttons->addButton(QStringLiteral("复制预览"), QDialogButtonBox::ActionRole);
    QObject::connect(copy, &QPushButton::clicked, &dialog, [preview] {
        QApplication::clipboard()->setText(preview->toPlainText());
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return false;

    const auto target = QFileDialog::getSaveFileName(parent, QStringLiteral("保存诊断报告"),
        root + "/EZ-诊断.txt", QStringLiteral("文本 (*.txt)"));
    if (target.isEmpty())
        return false;
    QSaveFile file(checkedPath(target));
    const auto contents = preview->toPlainText().toUtf8() + '\n';
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit())
        throw std::runtime_error(file.errorString().toStdString());
    return true;
}

}
