#include "engine.h"
#include "jobs.h"
#include "settings.h"
#include "synthesis.h"
#include "training.h"

#include <QApplication>
#include <QCloseEvent>
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
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSignalBlocker>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextStream>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <atomic>
#include <stdexcept>

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
    const auto path = ez::checkedWorkspaceRoot(requested);
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

void startProbeJob(QProcess &worker, const QString &engine, const QString &root, const QString &requestPath)
{
    QFile source(ez::checkedPath(requestPath));
    if (!source.open(QIODevice::ReadOnly))
        throw std::runtime_error("Could not read the test job request.");
    auto request = QJsonDocument::fromJson(source.readAll()).object();
    if (request.value("operation").toString().isEmpty())
        throw std::runtime_error("The test request must name an operation.");

    ez::startJobProcess(worker, engine, root, root + "/probe-job", request);
}

class Workspace final : public QWidget {
public:
    Workspace(const QString &root, const QString &defaultOutput) : root_(root), defaultOutput_(defaultOutput)
    {
        setWindowTitle(QStringLiteral("EZ-GPT-SoVITS · 语音工作台"));
        resize(1000, 690);
        setMinimumSize(760, 520);
        setStyleSheet(QStringLiteral(
            "QWidget { background:#101826; color:#e8edf6; font-size:14px; }"
            "QPushButton, QComboBox { background:#233249; border:1px solid #3b4f6b;"
            " border-radius:7px; padding:8px 14px; }"
            "QPushButton:hover { background:#304765; }"
            "QPushButton:disabled { color:#74849c; background:#192433; }"
            "QPlainTextEdit { background:#0b121e; border:1px solid #33455e; border-radius:8px; padding:12px; }"
            "QLabel#heading { font-size:27px; font-weight:700; }"
            "QLabel#muted { color:#a5b5ca; }"
            "QPushButton#primary { background:#245771; }"
        ));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(32, 26, 32, 22);
        layout->setSpacing(14);
        auto *title = new QLabel(QStringLiteral("语音工作台"), this);
        title->setObjectName("heading");
        layout->addWidget(title);
        auto *tabs = new QTabWidget(this);
        layout->addWidget(tabs, 1);
        auto *home = new QWidget(tabs);
        tabs->addTab(home, QStringLiteral("工作台"));
        auto *homeLayout = new QVBoxLayout(home);
        engineLocation_ = new QLabel(home);
        engineLocation_->setObjectName("muted");
        engineLocation_->setWordWrap(true);
        homeLayout->addWidget(engineLocation_);
        auto *paths = new QHBoxLayout;
        auto *existing = new QPushButton(QStringLiteral("使用已有模型"), home);
        auto *newVoice = new QPushButton(QStringLiteral("用录音训练声音"), home);
        paths->addWidget(existing);
        paths->addWidget(newVoice);
        auto *configure = new QPushButton(QStringLiteral("引擎设置"), home);
        paths->addWidget(configure);
        paths->addStretch();
        homeLayout->addLayout(paths);

        auto *selector = new QHBoxLayout;
        family_ = new QComboBox(this);
        family_->addItems({"v5turbo", "v5dev", "v2Pro", "v2ProPlus", "v2"});
        selector->addWidget(new QLabel(QStringLiteral("检查模型家族"), home));
        selector->addWidget(family_);
        selector->addStretch();
        homeLayout->addLayout(selector);

        auto *actions = new QHBoxLayout;
        check_ = new QPushButton(QStringLiteral("检查资源"), this);
        auto *digest = new QPushButton(QStringLiteral("计算文件 SHA-256"), this);
        auto *cancelCheck = new QPushButton(QStringLiteral("取消校验"), this);
        cancelCheck->setEnabled(false);
        start_ = new QPushButton(QStringLiteral("启动引擎"), this);
        start_->setObjectName("primary");
        auto *log = new QPushButton(QStringLiteral("打开启动日志"), this);
        auto *diagnostic = new QPushButton(QStringLiteral("导出诊断"), this);
        actions->addWidget(check_);
        actions->addWidget(digest);
        actions->addWidget(cancelCheck);
        actions->addWidget(start_);
        actions->addWidget(log);
        actions->addWidget(diagnostic);
        actions->addStretch();
        homeLayout->addLayout(actions);

        report_ = new QPlainTextEdit(this);
        report_->setPlaceholderText(QStringLiteral("资源检查结果会显示在这里"));
        report_->setReadOnly(true);
        homeLayout->addWidget(report_, 1);
        status_ = new QLabel(QStringLiteral("引擎未启动"), this);
        status_->setWordWrap(true);
        status_->setObjectName("muted");
        layout->addWidget(status_);

        auto *settings = createSettings(tabs);
        scheduler_ = new ez::TaskScheduler(root_, defaultOutput_, engine_, this);
        auto *synthesis = new ez::SynthesisPanel(root_, *scheduler_, [this] { return currentLauncher(); }, tabs);
        tabs->addTab(synthesis, QStringLiteral("语音合成"));
        auto *training = new ez::TrainingPanel(root_, *scheduler_, [this] { return currentLauncher(); },
            [tabs, synthesis](const QJsonObject &settings) {
                tabs->setCurrentWidget(synthesis);
                synthesis->importVoice(settings);
            }, tabs);
        tabs->addTab(training, QStringLiteral("训练项目"));
        tabs->addTab(createAssets(tabs, synthesis), QStringLiteral("声音与素材"));
        tabs->addTab(createHistory(tabs, synthesis), QStringLiteral("任务记录"));
        tabs->addTab(settings, QStringLiteral("设置"));
        connect(existing, &QPushButton::clicked, tabs, [tabs, synthesis] { tabs->setCurrentWidget(synthesis); });
        connect(newVoice, &QPushButton::clicked, tabs, [tabs, training] { tabs->setCurrentWidget(training); });

        connect(configure, &QPushButton::clicked, tabs, [tabs, settings] { tabs->setCurrentWidget(settings); });
        connect(check_, &QPushButton::clicked, this, &Workspace::checkEngine);
        connect(digest, &QPushButton::clicked, this, [this, digest, cancelCheck] {
            if (inspection_.isRunning())
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
            cancelled_.store(false);
            digest->setEnabled(false);
            cancelCheck->setEnabled(true);
            report_->setPlainText(QStringLiteral("正在计算 SHA-256…"));
            inspection_.setFuture(QtConcurrent::run([this, chosen, expected] {
                try {
                    QFile file(ez::checkedPath(chosen));
                    if (!file.open(QIODevice::ReadOnly))
                        return file.errorString();
                    QCryptographicHash hash(QCryptographicHash::Sha256);
                    while (!file.atEnd()) {
                        if (cancelled_.load())
                            return QStringLiteral("已取消校验");
                        const auto block = file.read(1024 * 1024);
                        if (file.error() != QFileDevice::NoError)
                            return file.errorString();
                        hash.addData(block);
                    }
                    const auto actual = QString::fromLatin1(hash.result().toHex());
                    const auto status = expected.isEmpty() ? QStringLiteral("已计算摘要，缺少同版本校验依据")
                        : expected == actual ? QStringLiteral("摘要匹配") : QStringLiteral("内容不匹配");
                    return QStringLiteral("%1\n%2\n%3").arg(status, chosen, actual);
                } catch (const std::exception &error) {
                    return failureMessage(error);
                }
            }));
            refreshActions();
        });
        connect(cancelCheck, &QPushButton::clicked, this, [this] { cancelled_.store(true); });
        connect(start_, &QPushButton::clicked, this, &Workspace::toggleEngine);
        connect(log, &QPushButton::clicked, this, [this] {
            if (QFileInfo::exists(logPath()))
                QDesktopServices::openUrl(QUrl::fromLocalFile(logPath()));
            else
                status_->setText(QStringLiteral("启动引擎后可查看日志"));
        });
        connect(diagnostic, &QPushButton::clicked, this, [this] {
            const auto target = QFileDialog::getSaveFileName(this, QStringLiteral("导出诊断"),
                root_ + "/EZ-诊断.txt", "Text (*.txt)");
            if (target.isEmpty())
                return;
            try {
                QSaveFile file(ez::checkedPath(target));
                const auto details = QStringLiteral("EZ-GPT-SoVITS %1\n引擎状态 %2\n\n%3\n")
                    .arg(qApp->applicationVersion(), status_->text(), report_->toPlainText()).toUtf8();
                if (!file.open(QIODevice::WriteOnly) || file.write(details) != details.size() || !file.commit())
                    throw std::runtime_error(file.errorString().toStdString());
                status_->setText(QStringLiteral("已保存诊断报告"));
            } catch (const std::exception &error) {
                status_->setText(failureMessage(error));
            }
        });
        connect(&engine_, &ez::EngineManager::statusChanged, this, [this](const QString &message) {
            status_->setText(message);
            refreshActions();
        });
        connect(profiles_, &QComboBox::currentIndexChanged, this, [this] {
            try {
                if (!currentLauncher().isEmpty())
                    ez::setWorkspaceSetting("currentEngine", currentLauncher());
                refreshActions();
            } catch (const std::exception &error) {
                status_->setText(failureMessage(error));
            }
        });
        connect(&inspection_, &QFutureWatcher<QString>::finished, this, [this] {
            report_->setPlainText(inspection_.result());
            refreshActions();
        });
        connect(&inspection_, &QFutureWatcher<QString>::finished, this, [digest, cancelCheck] {
            digest->setEnabled(true);
            cancelCheck->setEnabled(false);
        });
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
    QFutureWatcher<QString> inspection_;
    std::atomic_bool cancelled_ = false;
    QComboBox *profiles_ = nullptr;
    QComboBox *family_ = nullptr;
    QLineEdit *enginePath_ = nullptr;
    QLabel *engineLocation_ = nullptr;
    QPlainTextEdit *report_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *check_ = nullptr;
    QPushButton *start_ = nullptr;
    QPushButton *addEngine_ = nullptr;
    ez::TaskScheduler *scheduler_ = nullptr;

    QString logPath() const { return root_ + "/engine-start.log"; }
    QString currentLauncher() const { return profiles_->currentData().toString(); }

    QWidget *createSettings(QWidget *parent)
    {
        auto *page = new QWidget(parent);
        auto *form = new QFormLayout(page);
        auto *options = new QHBoxLayout;
        profiles_ = new QComboBox(page);
        profiles_->setPlaceholderText(QStringLiteral("尚未配置独立引擎"));
        addEngine_ = new QPushButton(QStringLiteral("添加启动路径"), page);
        options->addWidget(profiles_, 1);
        options->addWidget(addEngine_);
        form->addRow(QStringLiteral("GPT-SoVITS 引擎"), options);

        enginePath_ = new QLineEdit(page);
        enginePath_->setReadOnly(true);
        enginePath_->setPlaceholderText(QStringLiteral("请选择独立整合包中的 go-webui.bat"));
        form->addRow(QStringLiteral("引擎启动文件"), enginePath_);
        const auto savedOutput = ez::workspaceSetting("outputRoot");
        auto *output = new QLineEdit(QDir::toNativeSeparators(savedOutput.isEmpty() ? defaultOutput_ : savedOutput), page);
        output->setReadOnly(true);
        form->addRow(QStringLiteral("成品生成目录"), output);

        auto *actions = new QHBoxLayout;
        auto *choose = new QPushButton(QStringLiteral("更改目录"), page);
        auto *reset = new QPushButton(QStringLiteral("恢复默认目录"), page);
        auto *open = new QPushButton(QStringLiteral("打开成品目录"), page);
        actions->addWidget(choose);
        actions->addWidget(reset);
        actions->addWidget(open);
        actions->addStretch();
        form->addRow(actions);
        auto *dataFolder = new QLineEdit(QDir::toNativeSeparators(root_), page);
        dataFolder->setReadOnly(true);
        form->addRow(QStringLiteral("素材和项目数据"), dataFolder);

        const auto saveOutput = [this, output](const QString &selected) {
            try {
                const auto path = ez::checkedWorkspaceRoot(selected.isEmpty() ? defaultOutput_ : selected);
                if (!QDir().mkpath(path) || !QFileInfo(path).isWritable())
                    throw std::runtime_error(QStringLiteral("此目录无法写入，请选择可写目录").toStdString());
                ez::setWorkspaceSetting("outputRoot", selected.isEmpty() ? QStringLiteral("") : path);
                output->setText(QDir::toNativeSeparators(path));
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
        return page;
    }

    void openFolder(const QString &location)
    {
        const auto path = ez::checkedWorkspaceRoot(location);
        if (!QDir().mkpath(path) || !QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
            throw std::runtime_error(QStringLiteral("无法打开目录：%1").arg(path).toStdString());
    }

    QWidget *createAssets(QTabWidget *tabs, ez::SynthesisPanel *synthesis)
    {
        auto *page = new QWidget(tabs);
        auto *layout = new QVBoxLayout(page);
        auto *table = new QTableWidget(0, 4, page);
        table->setHorizontalHeaderLabels({QStringLiteral("声音"), QStringLiteral("模型家族"),
                                         QStringLiteral("参考语言"), QStringLiteral("验证引擎")});
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table);
        auto *actions = new QHBoxLayout;
        auto *import = new QPushButton(QStringLiteral("导入声音"), page);
        actions->addWidget(import);
        connect(import, &QPushButton::clicked, page, [tabs, synthesis] {
            tabs->setCurrentWidget(synthesis);
            synthesis->importVoice();
        });
        const QMap<QString, QString> folders{{QStringLiteral("模型目录"), "/assets/models"},
            {QStringLiteral("参考音频目录"), "/assets/audio"}, {QStringLiteral("成品目录"), "/outputs"},
            {QStringLiteral("训练项目目录"), "/projects"}};
        for (auto entry = folders.cbegin(); entry != folders.cend(); ++entry) {
            auto *open = new QPushButton(entry.key(), page);
            actions->addWidget(open);
            const auto folder = entry.value();
            connect(open, &QPushButton::clicked, page, [this, folder] {
                try {
                    openFolder(folder == "/outputs" ? ez::configuredOutputRoot(defaultOutput_) : root_ + folder);
                } catch (const std::exception &error) {
                    status_->setText(failureMessage(error));
                }
            });
        }
        actions->addStretch();
        layout->addLayout(actions);
        connect(tabs, &QTabWidget::currentChanged, page, [tabs, page, table] {
            if (tabs->currentWidget() != page)
                return;
            QSqlQuery query("SELECT name, settings, verified_engine FROM voices ORDER BY name");
            table->setRowCount(0);
            while (query.next()) {
                const int row = table->rowCount();
                const auto settings = QJsonDocument::fromJson(query.value(1).toByteArray()).object();
                const auto verified = query.value(2).toString();
                const QStringList fields{query.value(0).toString(), settings.value("family").toString(),
                    settings.value("prompt_language").toString(), verified.isEmpty() ? QStringLiteral("待验证") : verified};
                table->insertRow(row);
                for (int column = 0; column < fields.size(); ++column)
                    table->setItem(row, column, new QTableWidgetItem(fields[column]));
            }
        });
        return page;
    }

    QWidget *createHistory(QTabWidget *parent, ez::SynthesisPanel *synthesis)
    {
        auto *page = new QWidget(parent);
        auto *layout = new QVBoxLayout(page);
        auto *table = new QTableWidget(0, 4, page);
        table->setHorizontalHeaderLabels({QStringLiteral("创建时间"), QStringLiteral("操作"),
                                          QStringLiteral("状态"), QStringLiteral("结果")});
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table);
        auto *actions = new QHBoxLayout;
        auto *cancel = new QPushButton(QStringLiteral("取消所选任务"), page);
        auto *retry = new QPushButton(QStringLiteral("重试所选任务"), page);
        auto *open = new QPushButton(QStringLiteral("打开产物或日志目录"), page);
        auto *play = new QPushButton(QStringLiteral("播放成品"), page);
        auto *reuse = new QPushButton(QStringLiteral("复用合成参数"), page);
        actions->addWidget(cancel);
        actions->addWidget(retry);
        actions->addWidget(open);
        actions->addWidget(play);
        actions->addWidget(reuse);
        actions->addStretch();
        layout->addLayout(actions);
        const auto selection = [table] {
            const auto *item = table->item(table->currentRow(), 0);
            return item ? item->data(Qt::UserRole).toString() : QString();
        };
        const auto refresh = [table, selection] {
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
                table->item(row, 3)->setData(Qt::UserRole, audio);
                if (id == selected)
                    table->selectRow(row);
            }
        };
        refresh();
        connect(scheduler_, &ez::TaskScheduler::changed, page, refresh);
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
            const auto audio = table->item(table->currentRow(), 3)->data(Qt::UserRole).toString();
            try {
                const auto path = ez::checkedPath(audio.isEmpty() ? root_ + "/jobs/" + id : QFileInfo(audio).absolutePath());
                if (QFileInfo(path).isDir())
                    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
            } catch (const std::exception &error) {
                status_->setText(failureMessage(error));
            }
        });
        connect(play, &QPushButton::clicked, page, [this, parent, synthesis, table] {
            const auto *item = table->item(table->currentRow(), 3);
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
        return page;
    }

    void refreshActions()
    {
        const bool selected = !currentLauncher().isEmpty();
        enginePath_->setText(QDir::toNativeSeparators(currentLauncher()));
        engineLocation_->setText(selected ? QStringLiteral("当前引擎：%1").arg(QDir::toNativeSeparators(currentLauncher()))
            : QStringLiteral("请在「设置」中添加 GPT-SoVITS 引擎路径。"));
        check_->setEnabled(selected && !inspection_.isRunning());
        start_->setEnabled(selected || engine_.active());
        start_->setText(engine_.active() ? QStringLiteral("停止引擎") : QStringLiteral("启动引擎"));
        profiles_->setEnabled(!engine_.active());
        addEngine_->setEnabled(!engine_.active());
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
        refreshActions();
    }

    void addEngine()
    {
        const auto launcher = QFileDialog::getOpenFileName(this, QStringLiteral("选择独立整合包的 go-webui.bat"),
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

    void checkEngine()
    {
        if (inspection_.isRunning())
            return;
        report_->setPlainText(QStringLiteral("正在检查资源…"));
        const auto launcher = currentLauncher();
        const auto family = family_->currentText();
        const auto root = root_;
        inspection_.setFuture(QtConcurrent::run([launcher, family, root] {
            try {
                auto report = ez::inspectEngine(launcher, family);
                const QStorageInfo disk(root);
                report.append(QStringLiteral("数据磁盘：可用 %1 GiB").arg(disk.bytesAvailable() / 1073741824.0, 0, 'f', 1));
                return report.join("\n\n");
            } catch (const std::exception &error) {
                return failureMessage(error);
            }
        }));
        refreshActions();
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
        auto requested = parser.isSet(dataOption) ? parser.value(dataOption) : portableData;
        if (!parser.isSet(dataOption) && !QFileInfo::exists(ez::checkedPath(portableData + "/library.sqlite"))) {
            // Installations from before the portable default can still open their existing library.
            const auto previous = QSettings().value("dataRoot").toString();
            if (!previous.isEmpty()) {
                try {
                    const auto legacy = ez::checkedWorkspaceRoot(previous);
                    if (QFileInfo::exists(ez::checkedPath(legacy + "/library.sqlite")))
                        requested = legacy;
                } catch (const std::exception &) {
                    // A disconnected or disallowed legacy workspace must not block startup.
                }
            }
        }
        const auto root = prepareDataRoot(requested);
        const auto defaultOutput = (parser.isSet(dataOption) ? root : portableData) + "/outputs";
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
                auto remaining = requests.size();
                QObject::connect(&scheduler, &ez::TaskScheduler::completed, &app, [&] {
                    if (--remaining == 0)
                        app.exit(0);
                });
                QObject::connect(&scheduler, &ez::TaskScheduler::cancelled, &app, [&] {
                    if (--remaining == 0)
                        app.exit(0);
                });
                QObject::connect(&scheduler, &ez::TaskScheduler::failed, &app, [&app] { app.exit(3); });
                QObject::connect(&engine, &ez::EngineManager::statusChanged, &app, [&] {
                    if (!engine.active() && !scheduler.running())
                        app.exit(2);
                });
                QString first;
                for (const auto &value : requests) {
                    const auto request = value.toObject();
                    if (request.value("operation").toString().isEmpty())
                        return 3;
                    const auto id = scheduler.enqueue(parser.value(probeOption), request);
                    if (first.isEmpty())
                        first = id;
                }
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
                        startProbeJob(worker, engine.root(), root, parser.value(jobOption));
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
