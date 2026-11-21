#include "engine.h"
#include "jobs.h"
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
#include <QFutureWatcher>
#include <QHeaderView>
#include <QInputDialog>
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
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
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
    explicit Workspace(const QString &root) : root_(root)
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
        auto *hint = new QLabel(QStringLiteral("选择独立整合包中的 go-webui.bat，然后检查资源和启动引擎。"), home);
        hint->setObjectName("muted");
        hint->setWordWrap(true);
        homeLayout->addWidget(hint);

        auto *selector = new QHBoxLayout;
        profiles_ = new QComboBox(this);
        family_ = new QComboBox(this);
        family_->addItems({"v5turbo", "v5dev", "v2Pro", "v2ProPlus", "v2"});
        auto *add = new QPushButton(QStringLiteral("添加引擎"), this);
        selector->addWidget(profiles_, 1);
        selector->addWidget(family_);
        selector->addWidget(add);
        homeLayout->addLayout(selector);

        auto *actions = new QHBoxLayout;
        check_ = new QPushButton(QStringLiteral("检查资源"), this);
        auto *digest = new QPushButton(QStringLiteral("计算文件 SHA-256"), this);
        auto *cancelCheck = new QPushButton(QStringLiteral("取消校验"), this);
        cancelCheck->setEnabled(false);
        start_ = new QPushButton(QStringLiteral("启动引擎"), this);
        start_->setObjectName("primary");
        auto *log = new QPushButton(QStringLiteral("打开启动日志"), this);
        actions->addWidget(check_);
        actions->addWidget(digest);
        actions->addWidget(cancelCheck);
        actions->addWidget(start_);
        actions->addWidget(log);
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

        scheduler_ = new ez::TaskScheduler(root_, engine_, this);
        auto *synthesis = new ez::SynthesisPanel(root_, *scheduler_, [this] { return currentLauncher(); }, tabs);
        tabs->addTab(synthesis, QStringLiteral("语音合成"));
        auto *training = new ez::TrainingPanel(root_, *scheduler_, [this] { return currentLauncher(); },
            [tabs, synthesis](const QJsonObject &settings) {
                tabs->setCurrentWidget(synthesis);
                synthesis->importVoice(settings);
            }, tabs);
        tabs->addTab(training, QStringLiteral("训练项目"));
        tabs->addTab(createAssets(tabs, synthesis), QStringLiteral("声音与素材"));
        tabs->addTab(createHistory(tabs), QStringLiteral("任务记录"));

        connect(add, &QPushButton::clicked, this, &Workspace::addEngine);
        connect(check_, &QPushButton::clicked, this, &Workspace::checkEngine);
        connect(digest, &QPushButton::clicked, this, [this, digest, cancelCheck] {
            if (inspection_.isRunning())
                return;
            const auto chosen = QFileDialog::getOpenFileName(this, QStringLiteral("选择要校验的文件"), root_);
            if (chosen.isEmpty())
                return;
            cancelled_.store(false);
            digest->setEnabled(false);
            cancelCheck->setEnabled(true);
            report_->setPlainText(QStringLiteral("正在计算 SHA-256…"));
            inspection_.setFuture(QtConcurrent::run([this, chosen] {
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
                    return QStringLiteral("已计算摘要，缺少同版本校验依据\n%1\n%2")
                        .arg(chosen, QString::fromLatin1(hash.result().toHex()));
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
        connect(&engine_, &ez::EngineManager::statusChanged, this, [this](const QString &message) {
            status_->setText(message);
            refreshActions();
        });
        connect(profiles_, &QComboBox::currentIndexChanged, this, [this] { refreshActions(); });
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
    ez::EngineManager engine_;
    QFutureWatcher<QString> inspection_;
    std::atomic_bool cancelled_ = false;
    QComboBox *profiles_ = nullptr;
    QComboBox *family_ = nullptr;
    QPlainTextEdit *report_ = nullptr;
    QLabel *status_ = nullptr;
    QPushButton *check_ = nullptr;
    QPushButton *start_ = nullptr;
    ez::TaskScheduler *scheduler_ = nullptr;

    QString logPath() const { return root_ + "/engine-start.log"; }
    QString currentLauncher() const { return profiles_->currentData().toString(); }

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
                    const auto path = ez::checkedPath(root_ + folder);
                    if (QDir().mkpath(path))
                        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
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

    QWidget *createHistory(QWidget *parent)
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
        actions->addWidget(cancel);
        actions->addWidget(retry);
        actions->addWidget(open);
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
        return page;
    }

    void refreshActions()
    {
        const bool selected = !currentLauncher().isEmpty();
        check_->setEnabled(selected && !inspection_.isRunning());
        start_->setEnabled(selected || engine_.active());
        start_->setText(engine_.active() ? QStringLiteral("停止引擎") : QStringLiteral("启动引擎"));
        profiles_->setEnabled(!engine_.active());
    }

    void reloadProfiles(const QString &selected = {})
    {
        QSqlQuery query("SELECT name, launcher FROM engines ORDER BY id");
        checkQuery(query);
        profiles_->clear();
        profiles_->setPlaceholderText(QStringLiteral("请选择 GPT-SoVITS 引擎"));
        while (query.next()) {
            profiles_->addItem(query.value(0).toString(), query.value(1).toString());
            profiles_->setItemData(profiles_->count() - 1, query.value(1), Qt::ToolTipRole);
        }
        profiles_->setCurrentIndex(selected.isEmpty() && profiles_->count() ? 0 : profiles_->findData(selected));
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
            reloadProfiles(path);
            checkEngine();
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
    app.setApplicationVersion("0.1.0");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption dataOption("data-root", "Set the directory for EZ data.", "directory");
    const QCommandLineOption probeOption("probe-engine", "Start, check and stop a test engine.", "go-webui.bat");
    const QCommandLineOption jobOption("probe-job", "Run a test job using a started test engine.", "request.json");
    const QCommandLineOption queueOption("probe-queue", "Run the persistent scheduler in command-line verification.");
    const QCommandLineOption smokeOption("smoke-test", "Show the workspace and quit after startup.");
    parser.addOptions({dataOption, probeOption, jobOption, queueOption, smokeOption});
    parser.process(app);

    try {
        const auto location = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/EZ-GPT-SoVITS";
        QString requested;
        if (parser.isSet(dataOption)) {
            requested = parser.value(dataOption);
        } else {
            QSettings settings;
            requested = settings.value("dataRoot").toString();
            if (requested.isEmpty()) {
                bool accepted = false;
                requested = QInputDialog::getText(nullptr, QStringLiteral("选择数据目录"),
                    QStringLiteral("素材、训练项目和生成的音频会保存在此目录。"),
                    QLineEdit::Normal, location, &accepted);
                if (!accepted)
                    return 0;
                requested = prepareDataRoot(requested);
                settings.setValue("dataRoot", requested);
            }
        }
        const auto root = prepareDataRoot(requested);
        QLockFile lock(ez::checkedPath(root + "/workspace.lock"));
        if (!lock.tryLock())
            throw std::runtime_error(QStringLiteral("此数据目录已由另一个 EZ 实例使用").toStdString());
        initDatabase(root);
        if (parser.isSet(probeOption)) {
            ez::EngineManager engine;
            if (parser.isSet(queueOption)) {
                ez::TaskScheduler scheduler(root, engine);
                QFile source(ez::checkedPath(parser.value(jobOption)));
                if (!source.open(QIODevice::ReadOnly))
                    return 3;
                const auto request = QJsonDocument::fromJson(source.readAll()).object();
                if (request.value("operation").toString().isEmpty())
                    return 3;
                QObject::connect(&scheduler, &ez::TaskScheduler::completed, &app, [&app] { app.exit(0); });
                QObject::connect(&scheduler, &ez::TaskScheduler::failed, &app, [&app] { app.exit(3); });
                QObject::connect(&engine, &ez::EngineManager::statusChanged, &app, [&] {
                    if (!engine.active() && !scheduler.running())
                        app.exit(2);
                });
                scheduler.enqueue(parser.value(probeOption), request);
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
        Workspace window(root);
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
