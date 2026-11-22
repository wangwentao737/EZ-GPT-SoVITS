#include "diagnostics.h"
#include "engine.h"
#include "jobs.h"
#include "library.h"
#include "resources.h"
#include "settings.h"
#include "synthesis.h"
#include "training.h"
#include "ui.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLockFile>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTabWidget>
#include <QTableWidget>
#include <QTabBar>
#include <QTextStream>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <string_view>

namespace {

QString failureMessage(const std::exception &error)
{
    return QString::fromUtf8(error.what());
}

void checkQuery(const QSqlQuery &query)
{
    if (query.lastError().isValid())
        throw std::runtime_error(query.lastError().text().toStdString());
}

QString prepareDataRoot(const QString &requested)
{
    const auto path = ez::checkedPath(requested);
    if (!QDir().mkpath(path))
        throw std::runtime_error(QStringLiteral("无法创建数据目录：%1").arg(path).toStdString());
    return path;
}

void initDatabase(const QString &root)
{
    auto database = QSqlDatabase::addDatabase("QSQLITE");
    database.setDatabaseName(ez::checkedPath(root + "/library.sqlite"));
    if (!database.open())
        throw std::runtime_error(database.lastError().text().toStdString());
    QSqlQuery query;
    query.exec("CREATE TABLE IF NOT EXISTS engines ("
               "id INTEGER PRIMARY KEY, name TEXT NOT NULL, "
               "launcher TEXT NOT NULL COLLATE NOCASE UNIQUE)");
    checkQuery(query);
    ez::initWorkspaceSettings();
}

void startProbeJob(QProcess &worker, const ez::EngineManager &engine, const QString &root, const QString &requestPath)
{
    QFile source(ez::checkedPath(requestPath));
    if (!source.open(QIODevice::ReadOnly))
        throw std::runtime_error("Could not read the test job request.");
    auto request = QJsonDocument::fromJson(source.readAll()).object();
    if (request.value("operation").toString().isEmpty())
        throw std::runtime_error("The test request must name an operation.");

    ez::startJobProcess(worker, engine.root(), root, root + "/probe-job", request, engine.webuiPid());
}

struct InspectionResult {
    QList<ez::ResourceCheck> checks;
    QString heading;
    QString message;
    bool replace = false;
};

class Workspace final : public QWidget {
public:
    Workspace(const QString &root, const QString &defaultOutput) : root_(root), defaultOutput_(defaultOutput)
    {
        setWindowTitle(QStringLiteral("EZ-GPT-SoVITS · 语音工作台"));
        resize(1120, 740);
        setMinimumSize(900, 600);
        setObjectName("workspace");
        setStyleSheet(ez::ui::stylesheet());
        auto *shell = new QHBoxLayout(this);
        shell->setContentsMargins(0, 0, 0, 0);
        shell->setSpacing(0);
        auto *sidebar = new QWidget(this);
        sidebar->setObjectName("sidebar");
        sidebar->setFixedWidth(182);
        auto *rail = new QVBoxLayout(sidebar);
        rail->setContentsMargins(13, 21, 13, 16);
        rail->setSpacing(12);
        auto *brand = new QHBoxLayout;
        auto *mark = new QLabel(QStringLiteral("EZ"), sidebar);
        mark->setObjectName("brandMark");
        mark->setFixedSize(34, 34);
        mark->setAlignment(Qt::AlignCenter);
        auto *brandName = new QLabel(QStringLiteral("GPT-SoVITS"), sidebar);
        brandName->setObjectName("brand");
        brand->addWidget(mark);
        brand->addWidget(brandName, 1);
        rail->addLayout(brand);
        auto *navigation = new QListWidget(sidebar);
        navigation->setObjectName("navigation");
        navigation->setAccessibleName(QStringLiteral("页面导航"));
        rail->addWidget(navigation, 1);
        engineBadge_ = new QLabel(sidebar);
        engineBadge_->setObjectName("engineState");
        rail->addWidget(engineBadge_);
        auto *version = new QLabel(QStringLiteral("Windows  ·  %1").arg(qApp->applicationVersion()), sidebar);
        version->setObjectName("version");
        rail->addWidget(version);
        shell->addWidget(sidebar);

        auto *content = new QWidget(this);
        auto *layout = new QVBoxLayout(content);
        layout->setContentsMargins(25, 22, 25, 15);
        layout->setSpacing(10);
        shell->addWidget(content, 1);
        auto *tabs = new QTabWidget(content);
        tabs->tabBar()->hide();
        layout->addWidget(tabs, 1);
        auto *home = new QWidget(tabs);
        tabs->addTab(home, QStringLiteral("工作台"));
        auto *homeLayout = new QVBoxLayout(home);
        ez::ui::pageHeader(homeLayout, home, QStringLiteral("工作台"),
            QStringLiteral("首次使用先在设置添加 GPT-SoVITS 启动路径，再选择合成或训练。"));

        auto *paths = new QHBoxLayout;
        paths->setSpacing(11);
        const auto workflow = [home, paths](const QString &label, const QString &description) {
            auto *card = new QWidget(home);
            card->setObjectName("card");
            auto *stack = new QVBoxLayout(card);
            stack->setContentsMargins(13, 10, 13, 10);
            auto *button = new QPushButton(label, card);
            stack->addWidget(button, 0, Qt::AlignLeft);
            stack->addWidget(ez::ui::hint(description, card));
            paths->addWidget(card, 1);
            return button;
        };
        auto *existing = workflow(QStringLiteral("使用已有模型"), QStringLiteral("导入两份权重和参考录音，直接合成台词。"));
        auto *newVoice = workflow(QStringLiteral("用录音训练声音"), QStringLiteral("从录音切片、标注，训练并保存一个新声音。"));
        homeLayout->addLayout(paths);

        auto *engineTitle = new QLabel(QStringLiteral("引擎与资源"), home);
        engineTitle->setObjectName("sectionTitle");
        homeLayout->addWidget(engineTitle);
        engineLocation_ = ez::ui::hint({}, home);
        homeLayout->addWidget(engineLocation_);
        auto *selector = new QHBoxLayout;
        family_ = new QComboBox(this);
        family_->addItems({"v5turbo", "v5dev", "v2Pro", "v2ProPlus", "v2"});
        selector->addWidget(new QLabel(QStringLiteral("检查模型家族"), home));
        selector->addWidget(family_);
        checkVoice_ = new QComboBox(home);
        selector->addWidget(checkVoice_, 1);
        auto *configure = new QPushButton(QStringLiteral("引擎设置"), home);
        selector->addWidget(configure);
        homeLayout->addLayout(selector);

        auto *actions = new QHBoxLayout;
        check_ = new QPushButton(QStringLiteral("检查资源"), this);
        digest_ = new QPushButton(QStringLiteral("计算文件 SHA-256"), this);
        cancelCheck_ = new QPushButton(QStringLiteral("取消检查"), this);
        start_ = new QPushButton(QStringLiteral("启动引擎"), this);
        start_->setObjectName("primary");
        auto *log = new QPushButton(QStringLiteral("打开启动日志"), this);
        auto *diagnostic = new QPushButton(QStringLiteral("导出诊断"), this);
        actions->addWidget(start_);
        actions->addWidget(check_);
        actions->addWidget(digest_);
        actions->addWidget(cancelCheck_);
        actions->addWidget(log);
        actions->addWidget(diagnostic);
        actions->addStretch();
        homeLayout->addLayout(actions);

        auto *reportActions = new QHBoxLayout;
        failedCheck_ = new QComboBox(home);
        failedCheck_->setPlaceholderText(QStringLiteral("没有待复查项"));
        retryCheck_ = new QPushButton(QStringLiteral("复查所选项"), home);
        auto *copyReport = new QPushButton(QStringLiteral("复制报告"), home);
        reportActions->addWidget(failedCheck_, 1);
        reportActions->addWidget(retryCheck_);
        reportActions->addWidget(copyReport);
        homeLayout->addLayout(reportActions);

        report_ = new QPlainTextEdit(this);
        report_->setPlaceholderText(QStringLiteral("点「检查资源」查看可用能力。补齐缺失文件后，可选择失败项单独复查。"));
        report_->setReadOnly(true);
        homeLayout->addWidget(report_, 1);
        status_ = new QLabel(QStringLiteral("请先选择引擎，再开始合成或训练。"), content);
        status_->setWordWrap(true);
        status_->setTextFormat(Qt::PlainText);
        status_->setObjectName("statusLine");
        layout->addWidget(status_);

        auto *settings = createSettings(tabs);
        scheduler_ = new ez::TaskScheduler(root_, defaultOutput_, engine_, this);
        connect(scheduler_, &ez::TaskScheduler::failed, this, [this](const QString &message) {
            status_->setText(message.section('\n', 0, 0));
        });
        auto *synthesis = new ez::SynthesisPanel(root_, *scheduler_, [this] { return currentLauncher(); }, tabs);
        tabs->addTab(synthesis, QStringLiteral("语音合成"));
        auto *training = new ez::TrainingPanel(root_, *scheduler_, [this] { return currentLauncher(); },
            [tabs, synthesis](const QJsonObject &settings) {
                tabs->setCurrentWidget(synthesis);
                synthesis->importVoice(settings);
            }, tabs);
        tabs->addTab(training, QStringLiteral("训练项目"));
        tabs->addTab(ez::createLibraryPage(root_, defaultOutput_, *scheduler_, synthesis, tabs), QStringLiteral("声音与素材"));
        tabs->addTab(createHistory(tabs, synthesis), QStringLiteral("任务记录"));
        tabs->addTab(settings, QStringLiteral("设置"));
        for (int index = 0; index < tabs->count(); ++index)
            navigation->addItem(tabs->tabText(index));
        navigation->setCurrentRow(tabs->currentIndex());
        connect(navigation, &QListWidget::currentRowChanged, tabs, &QTabWidget::setCurrentIndex);
        connect(tabs, &QTabWidget::currentChanged, navigation, [navigation](int index) {
            navigation->setCurrentRow(index);
        });
        connect(existing, &QPushButton::clicked, tabs, [tabs, synthesis] { tabs->setCurrentWidget(synthesis); });
        connect(newVoice, &QPushButton::clicked, tabs, [tabs, training] { tabs->setCurrentWidget(training); });

        connect(configure, &QPushButton::clicked, tabs, [tabs, settings] { tabs->setCurrentWidget(settings); });
        connect(check_, &QPushButton::clicked, this, [this] { checkEngine({}); });
        connect(retryCheck_, &QPushButton::clicked, this, [this] {
            const auto id = failedCheck_->currentData().toString();
            if (!id.isEmpty())
                checkEngine(id);
        });
        connect(digest_, &QPushButton::clicked, this, &Workspace::checkDigest);
        connect(cancelCheck_, &QPushButton::clicked, this, [this] { cancelled_.store(true); });
        connect(copyReport, &QPushButton::clicked, this, [this] {
            QApplication::clipboard()->setText(report_->toPlainText());
        });
        connect(report_, &QPlainTextEdit::textChanged, copyReport, [this, copyReport] {
            copyReport->setEnabled(!report_->toPlainText().isEmpty());
        });
        copyReport->setEnabled(false);
        connect(family_, &QComboBox::currentTextChanged, this, [this] {
            reloadCheckVoices();
            resetReport();
        });
        connect(checkVoice_, &QComboBox::currentIndexChanged, this, [this] { resetReport(); });
        connect(tabs, &QTabWidget::currentChanged, this, [this, tabs, home] {
            if (tabs->currentWidget() == home)
                reloadCheckVoices();
        });
        connect(start_, &QPushButton::clicked, this, &Workspace::toggleEngine);
        connect(log, &QPushButton::clicked, this, [this] {
            if (QFileInfo::exists(logPath()))
                QDesktopServices::openUrl(QUrl::fromLocalFile(logPath()));
            else
                status_->setText(QStringLiteral("启动引擎后可查看日志"));
        });
        connect(diagnostic, &QPushButton::clicked, this, [this] { showDiagnostics(); });
        connect(&engine_, &ez::EngineManager::statusChanged, this, [this](const QString &message) {
            status_->setText(message);
            refreshActions();
        });
        connect(profiles_, &QComboBox::currentIndexChanged, this, [this] {
            try {
                if (!currentLauncher().isEmpty())
                    ez::setWorkspaceSetting("currentEngine", currentLauncher());
                resetReport();
                refreshActions();
            } catch (const std::exception &error) {
                status_->setText(failureMessage(error));
            }
        });
        connect(&inspection_, &QFutureWatcher<InspectionResult>::finished, this, [this] {
            checking_ = false;
            const auto result = inspection_.result();
            if (result.checks.isEmpty()) {
                report_->setPlainText(result.message);
            } else {
                updateReport(result);
            }
            refreshActions();
        });
        reloadCheckVoices();
        reloadProfiles();
    }

    ~Workspace() override
    {
        cancelled_.store(true);
        inspection_.waitForFinished();
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        if (scheduler_->running()) {
            QMessageBox box(QMessageBox::Question, QStringLiteral("有任务正在运行"),
                            QStringLiteral("是否取消当前任务并退出？"), QMessageBox::NoButton, this);
            auto *keep = box.addButton(QStringLiteral("继续等待"), QMessageBox::RejectRole);
            box.addButton(QStringLiteral("取消任务并退出"), QMessageBox::AcceptRole);
            box.setDefaultButton(keep);
            box.exec();
            if (box.clickedButton() == keep || box.clickedButton() == nullptr) {
                event->ignore();
                return;
            }
        }
        scheduler_->stop();
        engine_.stop();
        event->accept();
    }

private:
    QString root_;
    QString defaultOutput_;
    ez::EngineManager engine_;
    QFutureWatcher<InspectionResult> inspection_;
    std::atomic_bool cancelled_ = false;
    bool checking_ = false;
    QList<ez::ResourceCheck> resourceChecks_;
    QString resourceReport_;
    QComboBox *profiles_ = nullptr;
    QComboBox *family_ = nullptr;
    QComboBox *checkVoice_ = nullptr;
    QComboBox *failedCheck_ = nullptr;
    QLineEdit *enginePath_ = nullptr;
    QLabel *engineLocation_ = nullptr;
    QLabel *engineBadge_ = nullptr;
    QPlainTextEdit *report_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *check_ = nullptr;
    QPushButton *digest_ = nullptr;
    QPushButton *cancelCheck_ = nullptr;
    QPushButton *retryCheck_ = nullptr;
    QPushButton *start_ = nullptr;
    QPushButton *addEngine_ = nullptr;
    ez::TaskScheduler *scheduler_ = nullptr;

    QString logPath() const { return root_ + "/engine-start.log"; }
    QString currentLauncher() const { return profiles_->currentData().toString(); }

    QWidget *createSettings(QWidget *parent)
    {
        auto *page = new QWidget(parent);
        auto *layout = new QVBoxLayout(page);
        ez::ui::pageHeader(layout, page, QStringLiteral("设置"),
            QStringLiteral("选择已安装的 GPT-SoVITS，并查看音频和项目的保存位置。"));
        const auto section = [page, layout](const QString &title, const QString &description) {
            auto *card = new QWidget(page);
            card->setObjectName("card");
            auto *stack = new QVBoxLayout(card);
            stack->setContentsMargins(15, 12, 15, 12);
            auto *label = new QLabel(title, card);
            label->setObjectName("sectionTitle");
            stack->addWidget(label);
            stack->addWidget(ez::ui::hint(description, card));
            auto *form = new QFormLayout;
            form->setContentsMargins(0, 0, 0, 0);
            form->setHorizontalSpacing(14);
            form->setVerticalSpacing(9);
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
            stack->addLayout(form);
            layout->addWidget(card);
            return form;
        };
        auto *engineForm = section(QStringLiteral("GPT-SoVITS 引擎"),
            QStringLiteral("点「添加启动路径」，选择整合包目录中的 go-webui.bat，再回工作台启动。"));
        auto *options = new QHBoxLayout;
        options->setSpacing(8);
        profiles_ = new QComboBox(page);
        profiles_->setPlaceholderText(QStringLiteral("尚未配置引擎"));
        addEngine_ = new QPushButton(QStringLiteral("添加启动路径"), page);
        options->addWidget(profiles_, 1);
        options->addWidget(addEngine_);
        engineForm->addRow(QStringLiteral("当前引擎"), options);

        enginePath_ = new QLineEdit(page);
        enginePath_->setReadOnly(true);
        enginePath_->setPlaceholderText(QStringLiteral("请选择已安装的 GPT-SoVITS 中的 go-webui.bat"));
        engineForm->addRow(QStringLiteral("引擎启动文件"), enginePath_);
        auto *outputForm = section(QStringLiteral("文件位置"),
            QStringLiteral("默认保存在程序同级的 data 文件夹。更改生成目录只影响随后提交的合成任务。"));
        const auto savedOutput = ez::workspaceSetting("outputRoot");
        auto *output = new QLineEdit(QDir::toNativeSeparators(savedOutput.isEmpty() ? defaultOutput_ : savedOutput), page);
        output->setReadOnly(true);
        output->setToolTip(output->text());
        outputForm->addRow(QStringLiteral("成品生成目录"), output);

        auto *actions = new QHBoxLayout;
        actions->setSpacing(8);
        auto *choose = new QPushButton(QStringLiteral("更改目录"), page);
        auto *reset = new QPushButton(QStringLiteral("恢复默认目录"), page);
        auto *open = new QPushButton(QStringLiteral("打开成品目录"), page);
        actions->addWidget(choose);
        actions->addWidget(reset);
        actions->addWidget(open);
        actions->addStretch();
        outputForm->addRow(actions);
        auto *dataFolder = new QLineEdit(QDir::toNativeSeparators(root_), page);
        dataFolder->setReadOnly(true);
        dataFolder->setToolTip(dataFolder->text());
        auto *openData = new QPushButton(QStringLiteral("打开数据目录"), page);
        auto *dataActions = new QHBoxLayout;
        dataActions->setSpacing(8);
        dataActions->addWidget(dataFolder, 1);
        dataActions->addWidget(openData);
        outputForm->addRow(QStringLiteral("素材和项目数据"), dataActions);
        layout->addStretch(1);

        const auto saveOutput = [this, output](const QString &selected) {
            try {
                const auto path = ez::checkedPath(selected.isEmpty() ? defaultOutput_ : selected);
                if (!QDir().mkpath(path) || !QFileInfo(path).isWritable())
                    throw std::runtime_error(QStringLiteral("此目录无法写入，请选择可写目录").toStdString());
                ez::setWorkspaceSetting("outputRoot", selected.isEmpty() ? QStringLiteral("") : path);
                output->setText(QDir::toNativeSeparators(path));
                output->setToolTip(output->text());
                status_->setText(QStringLiteral("生成目录已更新，新提交的合成任务将使用此目录。"));
            } catch (const std::exception &error) {
                status_->setText(failureMessage(error));
            }
        };
        connect(addEngine_, &QPushButton::clicked, this, &Workspace::addEngine);
        connect(choose, &QPushButton::clicked, page, [this, output, saveOutput] {
            const auto selected = QFileDialog::getExistingDirectory(this, QStringLiteral("选择成品生成目录"), output->text());
            if (!selected.isEmpty())
                saveOutput(selected);
        });
        connect(reset, &QPushButton::clicked, page, [saveOutput] { saveOutput({}); });
        connect(open, &QPushButton::clicked, page, [this] {
            try {
                openFolder(ez::configuredOutputRoot(defaultOutput_));
            } catch (const std::exception &error) {
                status_->setText(failureMessage(error));
            }
        });
        connect(openData, &QPushButton::clicked, page, [this] {
            try {
                openFolder(root_);
            } catch (const std::exception &error) {
                status_->setText(failureMessage(error));
            }
        });
        return page;
    }

    void openFolder(const QString &location)
    {
        const auto path = ez::checkedPath(location);
        if (!QDir().mkpath(path) || !QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
            throw std::runtime_error(QStringLiteral("无法打开目录：%1").arg(path).toStdString());
    }

    QWidget *createHistory(QTabWidget *parent, ez::SynthesisPanel *synthesis)
    {
        auto *page = new QWidget(parent);
        auto *layout = new QVBoxLayout(page);
        ez::ui::pageHeader(layout, page, QStringLiteral("任务记录"),
            QStringLiteral("左键选中一条任务。右键可查看产物或日志，并按任务状态选择播放、复用、重试或取消。"));
        auto *table = new QTableWidget(0, 4, page);
        table->setHorizontalHeaderLabels({QStringLiteral("创建时间"), QStringLiteral("操作"),
                                          QStringLiteral("状态"), QStringLiteral("结果")});
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        ez::ui::styleTable(table);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
        table->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table, 1);
        auto *empty = new QLabel(QStringLiteral("生成语音或开始训练后，任务会出现在这里。"), page);
        empty->setObjectName("emptyState");
        empty->setAlignment(Qt::AlignCenter);
        empty->setWordWrap(true);
        layout->addWidget(empty, 1);
        auto *actions = new QHBoxLayout;
        auto *cancel = new QPushButton(QStringLiteral("取消所选任务"), page);
        auto *retry = new QPushButton(QStringLiteral("重试所选任务"), page);
        auto *open = new QPushButton(QStringLiteral("产物或日志"), page);
        auto *play = new QPushButton(QStringLiteral("播放成品"), page);
        play->setObjectName("primary");
        auto *reuse = new QPushButton(QStringLiteral("复用合成参数"), page);
        auto *more = new QPushButton(QStringLiteral("更多操作"), page);
        auto *menu = new QMenu(more);
        auto *copyResult = menu->addAction(QStringLiteral("复制结果"));
        auto *exportJob = menu->addAction(QStringLiteral("导出所选任务诊断"));
        more->setMenu(menu);
        actions->addWidget(play);
        actions->addWidget(open);
        actions->addWidget(reuse);
        actions->addWidget(retry);
        actions->addWidget(cancel);
        actions->addWidget(more);
        actions->addStretch();
        layout->addLayout(actions);
        const auto selection = [table] {
            const auto *item = table->item(ez::ui::selectedRow(table), 0);
            return item ? item->data(Qt::UserRole).toString() : QString();
        };
        const auto refresh = [table, empty, selection] {
            const auto selected = selection();
            QSqlQuery query("SELECT id,created,operation,state,message,result FROM jobs ORDER BY created DESC,rowid DESC");
            const QMap<QString, QString> names{{"synthesize", QStringLiteral("语音合成")},
                {"slice", QStringLiteral("音频切片")}, {"asr", QStringLiteral("自动转写")},
                {"prepare", QStringLiteral("准备数据")}, {"train_gpt", QStringLiteral("训练 GPT")},
                {"train_sovits", QStringLiteral("训练 SoVITS")}};
            const QMap<QString, QString> states{{"queued", QStringLiteral("排队")}, {"running", QStringLiteral("运行")},
                {"succeeded", QStringLiteral("成功")}, {"failed", QStringLiteral("失败")},
                {"cancelled", QStringLiteral("已取消")}, {"interrupted", QStringLiteral("中断")}};
            table->setRowCount(0);
            while (query.next()) {
                const auto id = query.value(0).toString();
                const int row = table->rowCount();
                const auto time = QDateTime::fromString(query.value(1).toString(), Qt::ISODateWithMs).toLocalTime();
                const auto operation = query.value(2).toString();
                const auto state = query.value(3).toString();
                const auto result = QJsonDocument::fromJson(query.value(5).toByteArray()).object();
                const auto audio = result.value("audio").toString();
                table->insertRow(row);
                const QStringList columns{time.toString("yyyy-MM-dd HH:mm:ss"), names.value(operation, operation),
                                          states.value(state, state), audio.isEmpty() ? query.value(4).toString() : audio};
                for (int column = 0; column < columns.size(); ++column)
                    table->setItem(row, column, new QTableWidgetItem(columns[column]));
                table->item(row, 0)->setData(Qt::UserRole, id);
                table->item(row, 1)->setData(Qt::UserRole, operation);
                table->item(row, 2)->setData(Qt::UserRole, state);
                if (state == "succeeded")
                    table->item(row, 2)->setForeground(QColor("#9ae0c9"));
                else if (state == "failed" || state == "interrupted")
                    table->item(row, 2)->setForeground(QColor("#ffb6a6"));
                table->item(row, 3)->setData(Qt::UserRole, audio);
                table->item(row, 3)->setToolTip(columns[3]);
                if (id == selected)
                    table->selectRow(row);
            }
            table->setVisible(table->rowCount() > 0);
            empty->setVisible(table->rowCount() == 0);
        };
        refresh();
        const auto updateActions = [=] {
            const int row = ez::ui::selectedRow(table);
            const auto *operation = table->item(row, 1);
            const auto *status = table->item(row, 2);
            const auto *result = table->item(row, 3);
            const auto state = status ? status->data(Qt::UserRole).toString() : QString();
            play->setEnabled(result && !result->data(Qt::UserRole).toString().isEmpty());
            open->setEnabled(status != nullptr);
            reuse->setEnabled(operation && operation->data(Qt::UserRole).toString() == "synthesize");
            cancel->setEnabled(state == "queued" || state == "running");
            retry->setEnabled(state == "failed" || state == "interrupted" || state == "cancelled");
            more->setEnabled(status != nullptr);
            copyResult->setEnabled(result && !result->text().isEmpty());
            exportJob->setEnabled(status != nullptr);
        };
        updateActions();
        connect(table, &QTableWidget::itemSelectionChanged, page, updateActions);
        connect(scheduler_, &ez::TaskScheduler::changed, page, [refresh, updateActions] {
            refresh();
            updateActions();
        });
        connect(copyResult, &QAction::triggered, page, [table] {
            if (const auto *result = table->item(ez::ui::selectedRow(table), 3); result && !result->text().isEmpty())
                QApplication::clipboard()->setText(result->text());
        });
        connect(exportJob, &QAction::triggered, page, [this, selection] {
            if (const auto id = selection(); !id.isEmpty())
                showDiagnostics(id);
        });
        connect(cancel, &QPushButton::clicked, page, [this, selection] {
            if (const auto id = selection(); !id.isEmpty()) {
                try {
                    scheduler_->cancel(id);
                } catch (const std::exception &error) {
                    status_->setText(failureMessage(error));
                }
            }
        });
        connect(retry, &QPushButton::clicked, page, [this, selection] {
            QSqlQuery query;
            query.prepare("SELECT launcher,request,operation FROM jobs WHERE id=? AND state IN ('failed','interrupted','cancelled')");
            query.addBindValue(selection());
            if (!query.exec() || !query.next())
                return;
            if (query.value(2).toString() != "synthesize") {
                status_->setText(QStringLiteral("请从训练项目重跑该阶段"));
                return;
            }
            const auto launcher = query.value(0).toString();
            const auto request = QJsonDocument::fromJson(query.value(1).toByteArray()).object();
            query.finish();
            try {
                scheduler_->enqueue(launcher, request);
            } catch (const std::exception &error) {
                status_->setText(failureMessage(error));
            }
        });
        connect(open, &QPushButton::clicked, page, [this, table, selection] {
            const auto id = selection();
            if (id.isEmpty())
                return;
            const auto audio = table->item(ez::ui::selectedRow(table), 3)->data(Qt::UserRole).toString();
            try {
                const auto path = ez::checkedPath(audio.isEmpty() ? root_ + "/jobs/" + id : QFileInfo(audio).absolutePath());
                if (QFileInfo(path).isDir())
                    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            } catch (const std::exception &error) {
                status_->setText(failureMessage(error));
            }
        });
        connect(play, &QPushButton::clicked, page, [this, parent, synthesis, table] {
            const auto *item = table->item(ez::ui::selectedRow(table), 3);
            if (!item || item->data(Qt::UserRole).toString().isEmpty())
                return;
            try {
                synthesis->playAudio(item->data(Qt::UserRole).toString());
                parent->setCurrentWidget(synthesis);
            } catch (const std::exception &error) {
                status_->setText(failureMessage(error));
            }
        });
        connect(reuse, &QPushButton::clicked, page, [parent, synthesis, selection] {
            QSqlQuery query;
            query.prepare("SELECT request FROM jobs WHERE id=? AND operation='synthesize'");
            query.addBindValue(selection());
            if (query.exec() && query.next()) {
                synthesis->loadRequest(QJsonDocument::fromJson(query.value(0).toByteArray()).object());
                parent->setCurrentWidget(synthesis);
            }
        });
        ez::ui::enableRowMenu(table, {play, open, reuse, retry, cancel}, {copyResult, exportJob});
        return page;
    }

    void refreshActions()
    {
        const bool selected = !currentLauncher().isEmpty();
        enginePath_->setText(QDir::toNativeSeparators(currentLauncher()));
        enginePath_->setToolTip(enginePath_->text());
        engineLocation_->setText(selected ? QStringLiteral("当前引擎：%1").arg(QDir::toNativeSeparators(currentLauncher()))
            : QStringLiteral("请在「设置」中添加 GPT-SoVITS 引擎路径。"));
        engineBadge_->setText(engine_.ready() ? QStringLiteral("引擎可使用") : engine_.active() ? QStringLiteral("引擎正在启动")
            : selected ? QStringLiteral("引擎未启动") : QStringLiteral("引擎未配置"));
        check_->setEnabled(selected && !checking_);
        digest_->setEnabled(!checking_);
        cancelCheck_->setEnabled(checking_);
        retryCheck_->setEnabled(selected && !checking_ && failedCheck_->count() > 0);
        failedCheck_->setEnabled(retryCheck_->isEnabled());
        family_->setEnabled(!checking_);
        checkVoice_->setEnabled(!checking_);
        start_->setEnabled(selected || engine_.active());
        start_->setText(engine_.active() ? QStringLiteral("停止引擎") : QStringLiteral("启动引擎"));
        profiles_->setEnabled(!engine_.active() && !checking_);
        addEngine_->setEnabled(!engine_.active() && !checking_);
    }

    void reloadProfiles(const QString &selected = {})
    {
        QSqlQuery query("SELECT name, launcher FROM engines ORDER BY id");
        checkQuery(query);
        const QSignalBlocker block(profiles_);
        profiles_->clear();
        profiles_->setPlaceholderText(QStringLiteral("请选择 GPT-SoVITS 引擎"));
        while (query.next()) {
            profiles_->addItem(query.value(0).toString(), query.value(1).toString());
            profiles_->setItemData(profiles_->count() - 1, query.value(1), Qt::ToolTipRole);
        }
        const auto saved = selected.isEmpty() ? ez::workspaceSetting("currentEngine") : selected;
        const int match = profiles_->findData(saved);
        profiles_->setCurrentIndex(match >= 0 ? match : (profiles_->count() ? 0 : -1));
        resetReport();
        refreshActions();
    }

    void addEngine()
    {
        const auto launcher = QFileDialog::getOpenFileName(this, QStringLiteral("选择 GPT-SoVITS 的 go-webui.bat"),
                                                          {}, QStringLiteral("启动入口 (go-webui.bat)"));
        if (launcher.isEmpty())
            return;
        try {
            const auto root = ez::engineRoot(launcher);
            const auto path = ez::checkedPath(launcher);
            QSqlQuery query;
            query.prepare("INSERT OR IGNORE INTO engines(name, launcher) VALUES (?, ?)");
            query.addBindValue(QFileInfo(root).fileName());
            query.addBindValue(path);
            query.exec();
            checkQuery(query);
            ez::setWorkspaceSetting("currentEngine", path);
            reloadProfiles(path);
            status_->setText(QStringLiteral("引擎已保存，可回到工作台检查资源或启动。"));
        } catch (const std::exception &error) {
            QMessageBox::warning(this, QStringLiteral("无法添加引擎"), failureMessage(error));
        }
    }

    void reloadCheckVoices()
    {
        if (checking_)
            return;
        QSqlQuery query("SELECT id,name,settings FROM voices ORDER BY name");
        if (query.lastError().isValid())
            return;
        const auto oldVoice = checkVoice_->currentData();
        const auto oldSettings = checkVoice_->currentData(Qt::UserRole + 1);
        const QSignalBlocker block(checkVoice_);
        checkVoice_->clear();
        checkVoice_->addItem(QStringLiteral("检查声音预设（可选）"));
        while (query.next()) {
            const auto settings = query.value(2).toByteArray();
            if (QJsonDocument::fromJson(settings).object().value("family").toString() == family_->currentText()) {
                checkVoice_->addItem(query.value(1).toString(), query.value(0));
                checkVoice_->setItemData(checkVoice_->count() - 1, settings, Qt::UserRole + 1);
            }
        }
        checkVoice_->setCurrentIndex(qMax(0, checkVoice_->findData(oldVoice)));
        if (oldSettings != checkVoice_->currentData(Qt::UserRole + 1))
            resetReport();
    }

    void resetReport()
    {
        if (!resourceChecks_.isEmpty()) {
            resourceChecks_.clear();
            resourceReport_.clear();
            report_->clear();
        }
        failedCheck_->clear();
        refreshActions();
    }

    void updateReport(const InspectionResult &result)
    {
        if (result.replace)
            resourceChecks_.clear();
        for (const auto &check : result.checks) {
            const auto old = std::find_if(resourceChecks_.begin(), resourceChecks_.end(),
                                          [&](const ez::ResourceCheck &entry) { return entry.id == check.id; });
            if (old == resourceChecks_.end())
                resourceChecks_.append(check);
            else
                *old = check;
        }
        QStringList lines{result.heading};
        const auto selected = failedCheck_->currentData();
        failedCheck_->clear();
        for (const auto &entry : resourceChecks_) {
            lines << QStringLiteral("%1 · %2\n%3").arg(entry.passed ? QStringLiteral("通过") : QStringLiteral("需处理"),
                                                       entry.label, entry.detail);
            if (!entry.passed)
                failedCheck_->addItem(entry.label, entry.id);
        }
        if (failedCheck_->count() > 0)
            failedCheck_->setCurrentIndex(qMax(0, failedCheck_->findData(selected)));
        resourceReport_ = lines.join("\n\n");
        report_->setPlainText(resourceReport_);
    }

    void beginInspection(const QString &message, auto work)
    {
        cancelled_.store(false);
        checking_ = true;
        report_->setPlainText(message);
        inspection_.setFuture(QtConcurrent::run(std::move(work)));
        refreshActions();
    }

    void checkEngine(const QString &only)
    {
        if (checking_)
            return;
        try {
            const auto launcher = currentLauncher();
            const auto family = family_->currentText();
            const auto root = root_;
            const auto configured = ez::workspaceSetting("outputRoot");
            const auto output = configured.isEmpty() ? defaultOutput_ : configured;
            const auto voice = QJsonDocument::fromJson(checkVoice_->currentData(Qt::UserRole + 1).toByteArray()).object();
            const auto interfaces = engine_.interfaceIssues();
            auto devices = engine_.devices();
            if (devices.isEmpty())
                devices = engine_.ready() ? QStringLiteral("此引擎未报告设备信息") : QStringLiteral("启动引擎后可查看");
            const auto heading = QStringLiteral("引擎 · %1\n模型家族 · %2\n设备 · %3").arg(launcher, family, devices);
            beginInspection(QStringLiteral("正在检查资源…"), [this, launcher, family, root, output, voice, interfaces, only, heading] {
                const auto checks = ez::inspectResources(launcher, family, root, output, voice, interfaces, only, cancelled_);
                return InspectionResult{checks, heading, QStringLiteral("已取消资源检查"), only.isEmpty()};
            });
        } catch (const std::exception &error) {
            status_->setText(failureMessage(error));
        }
    }

    void checkDigest()
    {
        if (checking_)
            return;
        const auto chosen = QFileDialog::getOpenFileName(this, QStringLiteral("选择要校验的文件"), root_);
        if (chosen.isEmpty())
            return;
        bool accepted = false;
        const auto expected = QInputDialog::getText(this, QStringLiteral("参考摘要"),
            QStringLiteral("可填写可信来源提供的 SHA-256 摘要，留空则只计算文件摘要。"),
            QLineEdit::Normal, {}, &accepted).trimmed().toLower();
        if (!accepted)
            return;
        if (!expected.isEmpty() && !QRegularExpression("^[0-9a-f]{64}$").match(expected).hasMatch()) {
            status_->setText(QStringLiteral("参考摘要应为 64 位十六进制字符"));
            return;
        }
        beginInspection(QStringLiteral("正在计算 SHA-256…"), [this, chosen, expected] {
            InspectionResult result;
            try {
                QFile file(ez::checkedPath(chosen));
                if (!file.open(QIODevice::ReadOnly))
                    throw std::runtime_error(file.errorString().toStdString());
                QCryptographicHash hash(QCryptographicHash::Sha256);
                while (!file.atEnd() && !cancelled_.load()) {
                    const auto block = file.read(1024 * 1024);
                    if (file.error() != QFileDevice::NoError)
                        throw std::runtime_error(file.errorString().toStdString());
                    hash.addData(block);
                }
                if (cancelled_.load()) {
                    result.message = QStringLiteral("已取消校验");
                } else {
                    const auto actual = QString::fromLatin1(hash.result().toHex());
                    const auto status = expected.isEmpty() ? QStringLiteral("已计算摘要，缺少同版本校验依据")
                        : expected == actual ? QStringLiteral("摘要匹配") : QStringLiteral("内容不匹配");
                    result.message = QStringLiteral("%1\n%2\n%3").arg(status, chosen, actual);
                }
            } catch (const std::exception &error) {
                result.message = failureMessage(error);
            }
            return result;
        });
    }

    void showDiagnostics(const QString &job = {})
    {
        try {
            const auto state = engine_.ready() ? QStringLiteral("已就绪")
                : engine_.active() ? QStringLiteral("正在启动") : QStringLiteral("未启动");
            if (ez::exportDiagnostics(this, root_, state, resourceReport_, job))
                status_->setText(QStringLiteral("已保存诊断报告"));
        } catch (const std::exception &error) {
            status_->setText(failureMessage(error));
        }
    }

    void toggleEngine()
    {
        if (engine_.active()) {
            if (scheduler_->running()) {
                status_->setText(QStringLiteral("请先在任务记录取消运行中的任务"));
                return;
            }
            engine_.stop();
            return;
        }
        try {
            engine_.start(currentLauncher(), logPath());
        } catch (const std::exception &error) {
            status_->setText(failureMessage(error));
        }
    }
};

}

int main(int argc, char **argv)
{
    if (argc == 3 && std::string_view(argv[1]) == ez::engineBrowserNotification)
        return 0;
    QApplication app(argc, argv);
    app.setApplicationName("EZ-GPT-SoVITS");
    app.setOrganizationName("EZ-GPT-SoVITS");
    app.setApplicationVersion(QStringLiteral(EZ_APP_VERSION));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption dataOption("data-root", "Set the directory for EZ data.", "directory");
    const QCommandLineOption probeOption("probe-engine", "Start, check and stop a test engine.", "go-webui.bat");
    const QCommandLineOption jobOption("probe-job", "Run a test job using a started test engine.", "request.json");
    const QCommandLineOption queueOption("probe-queue", "Run the persistent scheduler in command-line verification.");
    const QCommandLineOption cancelOption("probe-cancel-file", "Cancel the first queue job when this verification file is created.", "path");
    const QCommandLineOption smokeOption("smoke-test", "Show the workspace and quit after startup.");
    parser.addOptions({dataOption, probeOption, jobOption, queueOption, cancelOption, smokeOption});
    parser.process(app);

    try {
        const auto portableData = QCoreApplication::applicationDirPath() + "/data";
        const auto requested = parser.isSet(dataOption) ? parser.value(dataOption) : portableData;
        const auto root = prepareDataRoot(requested);
        const auto defaultOutput = root + "/outputs";
        QLockFile lock(ez::checkedPath(root + "/workspace.lock"));
        if (!lock.tryLock())
            throw std::runtime_error(QStringLiteral("此数据目录已由另一个 EZ 实例使用").toStdString());
        initDatabase(root);
        if (parser.isSet(probeOption)) {
            ez::EngineManager engine;
            if (parser.isSet(queueOption)) {
                ez::TaskScheduler scheduler(root, defaultOutput, engine);
                QFile source(ez::checkedPath(parser.value(jobOption)));
                if (!source.open(QIODevice::ReadOnly))
                    return 3;
                const auto document = QJsonDocument::fromJson(source.readAll());
                const auto requests = document.isArray() ? document.array() : QJsonArray{document.object()};
                if (requests.isEmpty())
                    return 3;
                QStringList submitted;
                const auto checkBatch = [&app, &submitted] {
                    bool failed = false;
                    QSqlQuery query;
                    query.prepare("SELECT state FROM jobs WHERE id=?");
                    for (const auto &id : submitted) {
                        query.bindValue(0, id);
                        if (!query.exec() || !query.next()) {
                            app.exit(3);
                            return;
                        }
                        const auto state = query.value(0).toString();
                        query.finish();
                        if (state == "queued" || state == "running")
                            return;
                        failed |= state != "succeeded" && state != "cancelled";
                    }
                    if (!submitted.isEmpty())
                        app.exit(failed ? 3 : 0);
                };
                QObject::connect(&engine, &ez::EngineManager::statusChanged, &app, [&] {
                    if (!engine.active() && !scheduler.running())
                        app.exit(2);
                });
                QString first;
                for (const auto &value : requests) {
                    const auto request = value.toObject();
                    if (request.value("operation").toString().isEmpty())
                        return 3;
                    submitted << scheduler.enqueue(parser.value(probeOption), request);
                }
                first = submitted.first();
                QObject::connect(&scheduler, &ez::TaskScheduler::changed, &app, checkBatch, Qt::QueuedConnection);
                QTimer::singleShot(0, &app, checkBatch);
                QTimer cancellation;
                if (parser.isSet(cancelOption)) {
                    const auto marker = ez::checkedPath(parser.value(cancelOption));
                    QObject::connect(&cancellation, &QTimer::timeout, &app, [&, marker] {
                        if (!QFileInfo::exists(marker))
                            return;
                        cancellation.stop();
                        try {
                            scheduler.cancel(first);
                        } catch (const std::exception &error) {
                            QTextStream(stderr) << failureMessage(error) << Qt::endl;
                            app.exit(3);
                        }
                    });
                    cancellation.start(50);
                }
                return app.exec();
            }
            QProcess worker;
            QObject::connect(&worker, &QProcess::errorOccurred, &app, [&app] { app.exit(3); });
            QObject::connect(&worker, &QProcess::finished, &app,
                [&app](int code, QProcess::ExitStatus exit) { app.exit(code == 0 && exit == QProcess::NormalExit ? 0 : 3); });
            QObject::connect(&engine, &ez::EngineManager::statusChanged, &app, [&](const QString &message) {
                QFile result(root + "/probe-result.txt");
                if (result.open(QIODevice::Append | QIODevice::Text))
                    result.write(message.toUtf8() + '\n');
                if (engine.ready()) {
                    if (!parser.isSet(jobOption)) {
                        app.exit(0);
                        return;
                    }
                    try {
                        startProbeJob(worker, engine, root, parser.value(jobOption));
                    } catch (const std::exception &error) {
                        if (result.isOpen())
                            result.write(failureMessage(error).toUtf8() + '\n');
                        app.exit(3);
                    }
                } else if (!engine.active()) {
                    app.exit(2);
                }
            });
            engine.start(parser.value(probeOption), root + "/engine-start.log");
            return app.exec();
        }
        Workspace window(root, defaultOutput);
        window.show();
        if (parser.isSet(smokeOption))
            QTimer::singleShot(750, &app, &QApplication::quit);
        return app.exec();
    } catch (const std::exception &error) {
        if (parser.isSet(probeOption) || parser.isSet(smokeOption))
            QTextStream(stderr) << failureMessage(error) << Qt::endl;
        else
            QMessageBox::critical(nullptr, QStringLiteral("无法打开 EZ-GPT-SoVITS"), failureMessage(error));
        return 1;
    }
}
