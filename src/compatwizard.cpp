#include "compatdraft.h"
#include "compatibility.h"
#include "ui.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWizard>
#include <QtConcurrentRun>
#include <stdexcept>

namespace {
using namespace ez::adaptation;

struct ScanResult {
    Capture capture;
    QString error;
};

class Wizard final : public QWizard {
public:
    Wizard()
    {
        setWindowTitle(QStringLiteral("EZ · 整合包适配向导"));
        resize(980, 630);
        setMinimumSize(780, 550);
        setWizardStyle(QWizard::ModernStyle);
        setOption(QWizard::NoBackButtonOnStartPage);
        setButtonText(QWizard::BackButton, QStringLiteral("上一步"));
        setButtonText(QWizard::NextButton, QStringLiteral("下一步"));
        setButtonText(QWizard::CancelButton, QStringLiteral("关闭"));
        setButtonText(QWizard::FinishButton, QStringLiteral("导出待审资料"));
        setupPackage();
        setupCapture();
        setupDraft();
        connect(this, &QWizard::currentIdChanged, this, [this](int page) {
            if (page == 1)
                refreshCapturePage();
            if (page == 2)
                refreshFinish();
        });
        connect(&scan_, &QFutureWatcher<ScanResult>::finished, this, [this] {
            const auto result = scan_.result();
            scanButton_->setEnabled(true);
            quick_->setEnabled(true);
            button(QWizard::BackButton)->setEnabled(true);
            if (!result.error.isEmpty()) {
                captureText_->setPlainText(result.error);
                return;
            }
            capture_ = result.capture;
            QStringList report{QStringLiteral("主服务 %1 · Gradio %2")
                .arg(capture_.ports.main).arg(capture_.main.config.value("version").toString())};
            if (capture_.inferenceIssue.isEmpty())
                report << QStringLiteral("快速推理 %1 · Gradio %2")
                    .arg(capture_.ports.inference).arg(capture_.inference.config.value("version").toString());
            else
                report << capture_.inferenceIssue;
            report << QString();
            for (const auto &check : capture_.checks)
                report << check.label() + QStringLiteral(" · ")
                    + (check.issue.isEmpty() ? QStringLiteral("可进入实际验收") : check.issue);
            captureText_->setPlainText(report.join('\n'));
            fillChecks();
            button(QWizard::NextButton)->setEnabled(true);
        });
    }

    ~Wizard() override { scan_.waitForFinished(); }

protected:
    bool validateCurrentPage() override
    {
        if (currentId() == 0) {
            try {
                const auto path = ez::engineRoot(QDir(package_->text().trimmed()).filePath("go-webui.bat"));
                if (path != scanPath_ || adapter_->currentText() != scanAdapter_)
                    capture_ = {};
                scanPath_ = path;
                scanAdapter_ = adapter_->currentText();
                baselineList_ = loadBaseline(baseline_->text().trimmed());
                packageError_->clear();
            } catch (const std::exception &error) {
                packageError_->setText(QString::fromUtf8(error.what()));
                return false;
            }
        }
        return currentId() != 1 || (!scan_.isRunning() && !capture_.directory.isEmpty());
    }

    void accept() override
    {
        exportFiles(false);
    }

private:
    void setupPackage()
    {
        auto *page = new QWizardPage(this);
        page->setTitle(QStringLiteral("1. 选择整合包"));
        page->setSubTitle(QStringLiteral("使用可单独启动的整合包副本。选择包含 go-webui.bat 的文件夹。"));
        auto *form = new QFormLayout(page);
        package_ = pathField(form, QStringLiteral("整合包副本"), false);
        baseline_ = pathField(form, QStringLiteral("基准清单"), true);
        baseline_->setPlaceholderText(QStringLiteral("留空使用向导内置清单"));
        adapter_ = new QComboBox(page);
        adapter_->addItems(ez::supportedEngineAdapters());
        form->addRow(QStringLiteral("对照的适配器"), adapter_);
        form->addRow(ez::ui::hint(QStringLiteral("不确定时先选择 20261006。若旧版快速推理提示入参不匹配，再对照 0725-cu124。"
            " 基准清单可选上游 main 中最新的 registry-v1.json。"), page));
        auto *manual = new QPushButton(QStringLiteral("打开新手手册"), page);
        connect(manual, &QPushButton::clicked, this, [this] {
            const auto path = QDir(QCoreApplication::applicationDirPath()).filePath("compat/quickstart-zh.md");
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
                packageError_->setText(QStringLiteral("手册位于便携目录的 compat/quickstart-zh.md。"));
        });
        form->addRow(manual);
        packageError_ = ez::ui::hint({}, page);
        form->addRow(packageError_);
        setPage(0, page);
    }

    void setupCapture()
    {
        auto *page = new QWizardPage(this);
        page->setTitle(QStringLiteral("2. 采集并核对接口"));
        page->setSubTitle(QStringLiteral("手动启动副本的 WebUI。要核对合成，再在 WebUI 中打开快速推理。"));
        auto *layout = new QVBoxLayout(page);
        ports_ = ez::ui::hint({}, page);
        layout->addWidget(ports_);
        auto *actions = new QHBoxLayout;
        quick_ = new QCheckBox(QStringLiteral("同时采集快速推理"), page);
        quick_->setChecked(true);
        actions->addWidget(quick_);
        actions->addStretch();
        scanButton_ = new QPushButton(QStringLiteral("采集并核对"), page);
        scanButton_->setObjectName("primary");
        actions->addWidget(scanButton_);
        layout->addLayout(actions);
        captureText_ = new QPlainTextEdit(page);
        captureText_->setReadOnly(true);
        captureText_->setPlaceholderText(QStringLiteral("这里会列出匹配的功能、缺失资源和接口差异。接口核对通过后，仍需用工作台实际验收。"));
        layout->addWidget(captureText_, 1);
        connect(quick_, &QCheckBox::toggled, this, [this] {
            capture_ = {};
            captureText_->clear();
            button(QWizard::NextButton)->setEnabled(false);
        });
        connect(scanButton_, &QPushButton::clicked, this, [this] {
            capture_ = {};
            scanButton_->setEnabled(false);
            quick_->setEnabled(false);
            button(QWizard::BackButton)->setEnabled(false);
            button(QWizard::NextButton)->setEnabled(false);
            captureText_->setPlainText(QStringLiteral("正在检查脚本、服务归属和接口…"));
            scan_.setFuture(QtConcurrent::run([path = scanPath_, adapter = scanAdapter_, quick = quick_->isChecked()] {
                try {
                    return ScanResult{scan(path, adapter, quick), {}};
                } catch (const std::exception &error) {
                    return ScanResult{{}, QString::fromUtf8(error.what())};
                }
            }));
        });
        setPage(1, page);
    }

    void setupDraft()
    {
        auto *page = new QWizardPage(this);
        page->setTitle(QStringLiteral("3. 记录验收并导出"));
        page->setSubTitle(QStringLiteral("先导出本机测试清单，再在独立数据目录实测。通过或失败的项目都要填写证据。"));
        auto *layout = new QVBoxLayout(page);
        auto *form = new QFormLayout;
        id_ = new QLineEdit(page);
        id_->setPlaceholderText(QStringLiteral("例如 gpt-sovits-20270115-cu124"));
        name_ = new QLineEdit(page);
        name_->setPlaceholderText(QStringLiteral("可与整合包发布名称一致"));
        origin_ = new QLineEdit(page);
        origin_->setPlaceholderText(QStringLiteral("发布页链接或归档文件名"));
        form->addRow(QStringLiteral("整合包 ID"), id_);
        form->addRow(QStringLiteral("显示名称"), name_);
        form->addRow(QStringLiteral("公开来源"), origin_);
        layout->addLayout(form);
        checks_ = new QTableWidget(0, 4, page);
        checks_->setHorizontalHeaderLabels({QStringLiteral("操作"), QStringLiteral("接口与资源"), QStringLiteral("实测结果"), QStringLiteral("任务号或现象")});
        ez::ui::styleTable(checks_);
        checks_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
        checks_->setColumnWidth(0, 147);
        checks_->setColumnWidth(1, 133);
        checks_->setColumnWidth(2, 134);
        layout->addWidget(checks_, 1);
        checkDetails_ = ez::ui::hint({}, page);
        layout->addWidget(checkDetails_);
        connect(checks_, &QTableWidget::currentCellChanged, this, [this](int row) {
            checkDetails_->setText(row < 0 || row >= capture_.checks.size() ? QString()
                : capture_.checks.at(row).issue.simplified().left(200));
        });
        auto *actions = new QHBoxLayout;
        auto *localTest = new QPushButton(QStringLiteral("导出本机测试清单"), page);
        connect(localTest, &QPushButton::clicked, this, [this] { exportFiles(true); });
        actions->addWidget(localTest);
        auto *open = new QPushButton(QStringLiteral("打开导出目录"), page);
        open->setEnabled(false);
        connect(open, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(exported_)); });
        actions->addWidget(open);
        layout->addLayout(actions);
        exportError_ = ez::ui::hint({}, page);
        layout->addWidget(exportError_);
        openOutput_ = open;
        setPage(2, page);
    }

    QLineEdit *pathField(QFormLayout *form, const QString &label, bool file)
    {
        auto *value = new QLineEdit(this);
        auto *browse = new QPushButton(QStringLiteral("浏览…"), this);
        auto *row = new QHBoxLayout;
        row->addWidget(value, 1);
        row->addWidget(browse);
        form->addRow(label, row);
        connect(browse, &QPushButton::clicked, this, [this, value, file] {
            const auto picked = file
                ? QFileDialog::getOpenFileName(this, QStringLiteral("选择基准清单"), value->text(), QStringLiteral("JSON (*.json)"))
                : QFileDialog::getExistingDirectory(this, QStringLiteral("选择整合包副本"), value->text());
            if (!picked.isEmpty())
                value->setText(QDir::toNativeSeparators(picked));
        });
        return value;
    }

    void refreshCapturePage()
    {
        try {
            const auto ports = ez::enginePorts(scanPath_, scanAdapter_);
            ports_->setText(QStringLiteral("主服务端口 %1，快速推理端口 %2。").arg(ports.main).arg(ports.inference));
        } catch (const std::exception &error) {
            ports_->setText(QString::fromUtf8(error.what()));
        }
        if (capture_.directory.isEmpty())
            captureText_->clear();
        button(QWizard::NextButton)->setEnabled(!capture_.directory.isEmpty() && !scan_.isRunning());
    }

    void fillChecks()
    {
        checks_->setRowCount(0);
        for (const auto &check : capture_.checks) {
            const auto row = checks_->rowCount();
            checks_->insertRow(row);
            auto *operation = new QTableWidgetItem(check.label());
            operation->setFlags(operation->flags() & ~Qt::ItemIsEditable);
            checks_->setItem(row, 0, operation);
            auto *finding = new QTableWidgetItem(check.issue.isEmpty() ? QStringLiteral("可以实测") : QStringLiteral("有待处理项"));
            finding->setFlags(finding->flags() & ~Qt::ItemIsEditable);
            finding->setToolTip(check.issue);
            checks_->setItem(row, 1, finding);
            auto *result = new QComboBox(checks_);
            result->addItem(QStringLiteral("不纳入"), QString());
            result->addItem(QStringLiteral("待验收"), QStringLiteral("unverified"));
            if (check.issue.isEmpty())
                result->addItem(QStringLiteral("通过"), QStringLiteral("passed"));
            result->addItem(QStringLiteral("未通过"), QStringLiteral("failed"));
            if (check.operation == "start" && check.issue.isEmpty())
                result->setCurrentIndex(1);
            connect(result, &QComboBox::currentIndexChanged, this, &Wizard::refreshFinish);
            checks_->setCellWidget(row, 2, result);
            auto *evidence = new QLineEdit(checks_);
            evidence->setPlaceholderText(QStringLiteral("填任务号、音频摘要或错误信息"));
            checks_->setCellWidget(row, 3, evidence);
        }
    }

    void refreshFinish()
    {
        if (currentId() != 2)
            return;
        bool hasEntry = false;
        for (int row = 0; row < checks_->rowCount(); ++row) {
            const auto *result = qobject_cast<QComboBox *>(checks_->cellWidget(row, 2));
            hasEntry |= result && !result->currentData().toString().isEmpty();
        }
        button(QWizard::FinishButton)->setEnabled(hasEntry);
    }

    void exportFiles(bool local)
    {
        Draft draft{id_->text().trimmed(), name_->text().trimmed(), origin_->text().trimmed(), baselineList_, {}};
        for (int row = 0; row < checks_->rowCount(); ++row) {
            const auto result = qobject_cast<QComboBox *>(checks_->cellWidget(row, 2))->currentData().toString();
            if (result.isEmpty())
                continue;
            const auto evidence = qobject_cast<QLineEdit *>(checks_->cellWidget(row, 3))->text();
            draft.entries.append({row, result, evidence});
        }
        try {
            validateDraft(capture_, draft, local);
            const auto parent = QFileDialog::getExistingDirectory(this, QStringLiteral("选择导出资料的文件夹"), exported_);
            if (parent.isEmpty())
                return;
            exported_ = exportDraft(capture_, draft, parent, local);
            openOutput_->setEnabled(true);
            exportError_->setText(QStringLiteral("已导出到 %1").arg(QDir::toNativeSeparators(exported_)));
        } catch (const std::exception &error) {
            exportError_->setText(QString::fromUtf8(error.what()));
        }
    }

    QLineEdit *package_ = nullptr;
    QLineEdit *baseline_ = nullptr;
    QLineEdit *id_ = nullptr;
    QLineEdit *name_ = nullptr;
    QLineEdit *origin_ = nullptr;
    QComboBox *adapter_ = nullptr;
    QLabel *packageError_ = nullptr;
    QLabel *ports_ = nullptr;
    QLabel *checkDetails_ = nullptr;
    QLabel *exportError_ = nullptr;
    QCheckBox *quick_ = nullptr;
    QPushButton *scanButton_ = nullptr;
    QPushButton *openOutput_ = nullptr;
    QPlainTextEdit *captureText_ = nullptr;
    QTableWidget *checks_ = nullptr;
    QJsonObject baselineList_;
    QString scanPath_, scanAdapter_, exported_;
    Capture capture_;
    QFutureWatcher<ScanResult> scan_;
};

}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    app.setWindowIcon(QIcon(":/branding/app-icon.png"));
    ez::ui::applyTheme(ez::ui::Theme::Dark);
    Wizard wizard;
    wizard.show();
    return app.exec();
}
