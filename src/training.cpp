#include "training.h"
#include "workspacepaths.h"
#include "engine.h"
#include "jobs.h"
#include "recordings.h"
#include "resources.h"
#include "ui.h"

#include <QAction>
#include <QAudioDevice>
#include <QAudioOutput>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMediaDevices>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSpinBox>
#include <QSqlError>
#include <QSqlQuery>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTimer>
#include <QUuid>
#include <QtConcurrentRun>
#include <stdexcept>
#include <utility>

namespace ez {
namespace {

class AudioOutputChoice final : public QComboBox {
public:
    AudioOutputChoice(QAudioOutput &output, QWidget *parent) : QComboBox(parent), output_(output)
    {
        setObjectName("clipAudioOutput");
        setAccessibleName(QStringLiteral("试听输出设备"));
        ui::fitComboBox(this, 16);
        auto *devices = new QMediaDevices(this);
        connect(devices, &QMediaDevices::audioOutputsChanged, this, [this] { refresh(); });
        connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive)
                refresh();
        });
        connect(this, &QComboBox::currentIndexChanged, this, [this] { route(); });
        refresh();
    }

    void refresh()
    {
        const auto selected = currentData().toByteArray();
        const auto system = QMediaDevices::defaultAudioOutput();
        const auto available = QMediaDevices::audioOutputs();
        const auto label = system.isNull() ? QStringLiteral("无可用音频输出")
            : QStringLiteral("系统默认 · %1").arg(system.description());
        bool changed = count() != available.size() + 1 || itemText(0) != label;
        for (int index = 0; !changed && index < available.size(); ++index) {
            const auto &device = available.at(index);
            changed = itemData(index + 1).toByteArray() != device.id()
                || itemText(index + 1) != device.description();
        }
        if (changed) {
            const QSignalBlocker block(this);
            clear();
            addItem(label, QByteArray());
            for (const auto &device : available)
                addItem(device.description(), device.id());
            setCurrentIndex(qMax(0, findData(selected)));
        }
        route();
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QComboBox::showEvent(event);
        refresh();
    }

    void showPopup() override
    {
        refresh();
        QComboBox::showPopup();
    }

private:
    void route()
    {
        auto device = QMediaDevices::defaultAudioOutput();
        const auto selected = currentData().toByteArray();
        if (!selected.isEmpty()) {
            for (const auto &output : QMediaDevices::audioOutputs()) {
                if (output.id() == selected) {
                    device = output;
                    break;
                }
            }
        }
        if (output_.device() != device)
            output_.setDevice(device);
        setToolTip(currentText());
    }

    QAudioOutput &output_;
};

const QStringList fullTraining{"slice", "prepare", "train_sovits", "train_gpt"};

QString newId()
{
    return QUuid::createUuid().toString(QUuid::Id128);
}

void require(bool valid, const QString &message)
{
    if (!valid)
        throw std::runtime_error(message.toStdString());
}

void execute(QSqlQuery &query)
{
    if (!query.exec())
        throw std::runtime_error(query.lastError().text().toStdString());
}

QString directory(const QString &path)
{
    const auto value = checkedPath(path);
    require(QDir().mkpath(value), QStringLiteral("无法创建项目目录"));
    return value;
}

void invalidate(QJsonObject &project)
{
    QJsonObject prior;
    for (const auto &key : {"dataset", "sampleCount", "gpt", "sovits", "engine", "experiment"}) {
        if (project.contains(key))
            prior[key] = project.take(key);
    }
    if (prior.contains("dataset")) {
        auto versions = project.value("training_versions").toArray();
        versions.append(storedWorkspacePaths(prior));
        project["training_versions"] = versions;
    }
}

template <typename Work>
auto recordingTask(QWidget *owner, const QString &title, int total, const Work &work)
{
    std::atomic_bool cancelled = false;
    std::atomic_int completed = 0;
    QProgressDialog progress(title, QStringLiteral("取消"), 0, total, owner);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.setMinimumDuration(0);
    progress.setWindowModality(Qt::WindowModal);
    const auto future = QtConcurrent::run([&] { return work(cancelled, completed); });
    QFutureWatcher<decltype(work(cancelled, completed))> watcher;
    QEventLoop wait;
    QTimer timer;
    QObject::connect(&progress, &QProgressDialog::canceled, &wait, [&] { cancelled.store(true); });
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &wait, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &wait, [&] {
        if (!cancelled.load() && total > 0) {
            progress.setValue(completed.load());
            progress.setLabelText(QStringLiteral("%1\n已完成 %2 / %3 个文件").arg(title).arg(completed.load()).arg(total));
        }
    });
    watcher.setFuture(future);
    if (!future.isFinished()) {
        progress.show();
        timer.start(100);
        wait.exec();
    }
    return future.result();
}

bool advanceReview(const QKeyEvent &event)
{
    return (event.key() == Qt::Key_Return || event.key() == Qt::Key_Enter)
        && event.modifiers().testFlag(Qt::ControlModifier);
}

class ReviewDelegate final : public QStyledItemDelegate {
public:
    explicit ReviewDelegate(QPushButton *save, QWidget *parent) : QStyledItemDelegate(parent), save_(save) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::KeyPress && advanceReview(*static_cast<QKeyEvent *>(event))) {
            if (auto *editor = qobject_cast<QWidget *>(watched)) {
                emit commitData(editor);
                emit closeEditor(editor);
                save_->click();
                return true;
            }
        }
        return QStyledItemDelegate::eventFilter(watched, event);
    }

private:
    QPushButton *save_;
};

}

TrainingPanel::TrainingPanel(QString root, TaskScheduler &scheduler, std::function<QString()> launcher,
                             std::function<void(QJsonObject)> openVoice, QWidget *parent)
    : QWidget(parent), root_(std::move(root)), scheduler_(scheduler),
      launcher_(std::move(launcher)), openVoice_(std::move(openVoice))
{
    QSqlQuery query;
    query.prepare("CREATE TABLE IF NOT EXISTS projects(id TEXT PRIMARY KEY, name TEXT NOT NULL, state BLOB NOT NULL)");
    execute(query);

    auto *layout = new QVBoxLayout(this);
    ui::pageHeader(layout, this, QStringLiteral("训练项目"));
    auto *selection = new QHBoxLayout;
    projects_ = new QComboBox(this);
    projects_->setPlaceholderText(QStringLiteral("创建或选择训练项目"));
    projects_->setAccessibleName(QStringLiteral("选择训练项目"));
    ui::fitComboBox(projects_, 15);
    selection->addWidget(projects_, 1);
    layout->addLayout(selection);
    stage_ = ui::hint({}, this);
    stage_->setObjectName("stageHint");
    layout->addWidget(stage_);
    compatibilityStatus_ = ui::hint({}, this);
    layout->addWidget(compatibilityStatus_);
    compatibilityActions_ = ui::engineCompatibilityActions(this, [this] { emit engineDetailsRequested(); },
        [this] { emit engineRecheckRequested(); }, [this] { emit engineSwitchRequested(); });
    layout->addWidget(compatibilityActions_);
    status_ = ui::hint({}, this);
    const auto button = [this](QHBoxLayout *row, const QString &label, auto action) {
        auto *control = new QPushButton(label, this);
        row->addWidget(control);
        connect(control, &QPushButton::clicked, this, [this, action] {
            try {
                action();
            } catch (const std::exception &error) {
                status_->setText(QString::fromUtf8(error.what()));
            }
        });
        return control;
    };
    create_ = button(selection, QStringLiteral("新建项目"), [this] { addProject(); });
    auto *files = new QHBoxLayout;
    recordings_ = button(files, QStringLiteral("导入录音"), [this] { importAudio(false); });
    slice_ = button(files, QStringLiteral("切片"), [this] { submit("slice"); });
    button(files, QStringLiteral("导入已切片录音"), [this] { importAudio(true); });
    transcribe_ = button(files, QStringLiteral("中文转写"), [this] { submit("asr"); });
    button(files, QStringLiteral("导入标注"), [this] {
        require(!busy(), QStringLiteral("请等待当前阶段完成"));
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("导入已有标注"), {}, "List (*.list *.txt)");
        if (!path.isEmpty())
            loadLabels(path);
    });
    layout->addLayout(files);

    labels_ = new QTableWidget(0, 4, this);
    labels_->setHorizontalHeaderLabels({QStringLiteral("片段"), QStringLiteral("语言"), QStringLiteral("状态"), QStringLiteral("文本")});
    labels_->setAccessibleName(QStringLiteral("音频切片与标注"));
    labels_->setToolTip(QStringLiteral("双击文本开始校对，右键打开片段操作。"));
    ui::styleTable(labels_);
    labels_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Interactive);
    labels_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    labels_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    labels_->setColumnWidth(0, 230);
    labels_->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(labels_, 1);
    player_ = new QMediaPlayer(this);
    auto *audioOutput = new QAudioOutput(player_);
    player_->setAudioOutput(audioOutput);
    auto *outputChoice = new AudioOutputChoice(*audioOutput, this);

    auto *playback = ui::hint(QStringLiteral("选中一条片段可试听。"), this);
    playback->setObjectName("clipPlayback");
    playback->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *labelsActions = new QHBoxLayout;
    const auto selectedClip = [this] {
        const auto *item = labels_->item(ui::selectedRow(labels_), 0);
        return item ? item->data(Qt::UserRole).toString() : QString();
    };
    const auto playableClip = [selectedClip] {
        const auto path = checkedPath(selectedClip());
        const QFileInfo file(path);
        require(file.isFile() && file.isReadable() && file.size() > 0,
                QStringLiteral("无法读取片段：%1。请重新导入该片段或重新切片。").arg(file.fileName()));
        return QUrl::fromLocalFile(path);
    };
    auto *playClip = button(labelsActions, QStringLiteral("试听片段"), [=, this] {
        try {
            const auto source = playableClip();
            if (player_->source() == source && player_->playbackState() == QMediaPlayer::PlayingState) {
                player_->pause();
                playback->setText(QStringLiteral("已暂停试听。"));
                return;
            }
            outputChoice->refresh();
            require(!audioOutput->device().isNull(), QStringLiteral("没有可用的音频输出设备。请连接耳机或扬声器后重试。"));
            if (player_->source() == source && player_->playbackState() == QMediaPlayer::PausedState) {
                player_->play();
                return;
            }
            player_->stop();
            playback->setText(QStringLiteral("正在加载片段 · %1").arg(source.fileName()));
            player_->setSource({});
            player_->setSource(source);
            player_->play();
        } catch (const std::exception &error) {
            playback->setText(QString::fromUtf8(error.what()));
        }
    });
    playClip_ = playClip;
    auto *openClip = new QAction(QStringLiteral("用系统播放器打开"), this);
    connect(openClip, &QAction::triggered, this, [=, this] {
        try {
            const auto source = playableClip();
            player_->stop();
            require(QDesktopServices::openUrl(source), QStringLiteral("无法打开系统播放器。"));
            playback->setText(QStringLiteral("已发送到系统播放器 · %1").arg(source.fileName()));
        } catch (const std::exception &error) {
            playback->setText(QString::fromUtf8(error.what()));
        }
    });
    auto *editClip = button(labelsActions, QStringLiteral("编辑文本"), [this] {
        require(!busy(), QStringLiteral("请等待当前阶段完成"));
        if (auto *item = labels_->item(ui::selectedRow(labels_), 3)) {
            labels_->setCurrentItem(item);
            labels_->editItem(item);
        }
    });
    auto *excludeClip = button(labelsActions, QStringLiteral("排除片段"), [this] {
        require(!busy(), QStringLiteral("请等待当前阶段完成"));
        const int row = ui::selectedRow(labels_);
        if (row >= 0) {
            auto *state = labels_->item(row, 2);
            state->setText(state->text() == QStringLiteral("已排除") ? QStringLiteral("待校对") : QStringLiteral("已排除"));
            reviewsDirty_ = true;
            updateReviewFilter();
        }
    });
    const auto updateClipActions = [=, this] {
        const auto path = selectedClip();
        const bool selected = !path.isEmpty();
        const bool canEdit = selected && !busy();
        const bool playing = player_->playbackState() == QMediaPlayer::PlayingState
            && player_->source() == QUrl::fromLocalFile(path);
        playClip->setText(playing ? QStringLiteral("暂停试听") : QStringLiteral("试听片段"));
        playClip->setEnabled(selected);
        openClip->setEnabled(selected);
        editClip->setEnabled(canEdit);
        excludeClip->setEnabled(canEdit);
    };
    updateClipActions();
    connect(labels_, &QTableWidget::itemSelectionChanged, this, updateClipActions);
    connect(&scheduler_, &TaskScheduler::changed, this, updateClipActions);
    connect(player_, &QMediaPlayer::playbackStateChanged, this, [=, this](QMediaPlayer::PlaybackState state) {
        updateClipActions();
        if (state == QMediaPlayer::PlayingState) {
            playback->setText(QStringLiteral("正在试听 · %1").arg(player_->source().fileName()));
            playback->setToolTip(audioOutput->device().description());
        }
    });
    connect(player_, &QMediaPlayer::mediaStatusChanged, this, [playback](QMediaPlayer::MediaStatus state) {
        if (state == QMediaPlayer::EndOfMedia)
            playback->setText(QStringLiteral("试听结束。"));
    });
    connect(player_, &QMediaPlayer::errorOccurred, this, [playback](QMediaPlayer::Error, const QString &detail) {
        playback->setText(QStringLiteral("无法试听：%1。可右键选择「用系统播放器打开」。").arg(detail));
    });
    connect(projects_, &QComboBox::currentIndexChanged, this, [=, this] {
        player_->setSource({});
        playback->setText(QStringLiteral("选中一条片段可试听。"));
    });
    ui::enableRowMenu(labels_, {playClip, editClip, excludeClip}, {openClip});
    button(labelsActions, QStringLiteral("保存标注"), [this] { saveLabels(); });
    saveNext_ = button(labelsActions, QStringLiteral("保存并下一条"), [this] { saveAndAdvance(); });
    saveNext_->setToolTip(QStringLiteral("Ctrl+Enter 保存校对并移动到下一条可见片段"));
    labels_->setItemDelegate(new ReviewDelegate(saveNext_, labels_));
    labels_->installEventFilter(this);
    labels_->viewport()->installEventFilter(this);
    prepare_ = button(labelsActions, QStringLiteral("准备数据"), [this] {
        saveLabels();
        submit("prepare");
    });
    labelsActions->addStretch();
    layout->addLayout(labelsActions);
    auto *playbackRow = new QHBoxLayout;
    playbackRow->addWidget(playback, 1);
    pendingOnly_ = new QCheckBox(QStringLiteral("只看待校对"), this);
    auto *loop = new QCheckBox(QStringLiteral("循环试听"), this);
    playbackRow->addWidget(pendingOnly_);
    playbackRow->addWidget(loop);
    playbackRow->addWidget(outputChoice);
    connect(pendingOnly_, &QCheckBox::toggled, this, &TrainingPanel::updateReviewFilter);
    connect(loop, &QCheckBox::toggled, this, [this](bool enabled) {
        player_->setLoops(enabled ? QMediaPlayer::Infinite : QMediaPlayer::Once);
    });
    connect(labels_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (item->column() != 1 && item->column() != 3)
            return;
        reviewsDirty_ = true;
        auto *state = labels_->item(item->row(), 2);
        if (state && state->text() != QStringLiteral("已排除"))
            state->setText(QStringLiteral("待校对"));
    });
    layout->addLayout(playbackRow);

    auto *training = new QHBoxLayout;
    gpu_ = new QComboBox(this);
    gpu_->setObjectName("trainingDevice");
    gpu_->setAccessibleName(QStringLiteral("训练设备"));
    gpu_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    gpu_->setMinimumContentsLength(18);
    selectDevice(0);
    training->addWidget(gpu_, 1);
    batch_ = new QSpinBox(this);
    batch_->setPrefix(QStringLiteral("批量 "));
    batch_->setRange(1, 40);
    batch_->setMaximumWidth(96);
    training->addWidget(batch_);
    epochs_ = new QSpinBox(this);
    epochs_->setPrefix(QStringLiteral("轮数 "));
    epochs_->setRange(2, 50);
    epochs_->setValue(4);
    epochs_->setMaximumWidth(96);
    training->addWidget(epochs_);
    train_ = button(training, QStringLiteral("开始训练"), [this] {
        submit(nextTrainingStage());
    });
    resume_ = button(training, QStringLiteral("从检查点继续"), [this] {
        submit(nextTrainingStage(), true);
    });
    resume_->setVisible(false);
    connect(gpu_, &QComboBox::currentIndexChanged, this, [this] { showState(); });
    for (auto *control : {batch_, epochs_})
        connect(control, &QSpinBox::valueChanged, this, [this] { showState(); });
    preview_ = button(training, QStringLiteral("保存声音"), [this] { importVoice(); });
    training->addStretch();
    layout->addLayout(training);
    layout->addWidget(status_);

    connect(projects_, &QComboBox::currentIndexChanged, this, [this] {
        try {
            if (reviewsDirty_ && !project_.isEmpty()) {
                const auto choice = QMessageBox::question(this, QStringLiteral("保存校对修改"),
                    QStringLiteral("切换项目之前保存当前校对修改？"),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
                if (choice == QMessageBox::Cancel) {
                    const QSignalBlocker block(projects_);
                    projects_->setCurrentIndex(projects_->findData(project_.value("id").toString()));
                    return;
                }
                if (choice == QMessageBox::Save) {
                    auto saved = project_;
                    saved["reviews"] = reviewDrafts();
                    saveProject(saved);
                }
            }
            loadProject();
        } catch (const std::exception &error) {
            status_->setText(QString::fromUtf8(error.what()));
        }
    });
    connect(&scheduler_, &TaskScheduler::completed, this, [this](const QString &id, const QJsonObject &result) {
        try {
            complete(id, result);
        } catch (const std::exception &error) {
            status_->setText(QString::fromUtf8(error.what()));
        }
    });
    connect(&scheduler_, &TaskScheduler::changed, this, [this] { showState(); });
    connect(&scheduler_.compatibility(), &CompatibilityRegistry::changed, this, &TrainingPanel::refreshEngineSelection);
    connect(&scheduler_, &TaskScheduler::activityChanged, this, &TrainingPanel::updateStageVisibility);
    query.prepare("SELECT name,id FROM projects ORDER BY rowid DESC");
    execute(query);
    while (query.next())
        projects_->addItem(query.value(0).toString(), query.value(1).toString());
    if (projects_->count() > 0)
        projects_->setCurrentIndex(0);
    showState();
}

void TrainingPanel::addProject()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("新建训练项目"));
    QFormLayout form(&dialog);
    const auto installation = scheduler_.compatibility().inspect(launcher_());
    const auto available = installation.families(fullTraining);
    require(!available.isEmpty(), QStringLiteral("此整合包没有可完成全部训练阶段的模型家族。请查看引擎详情。"));
    auto *name = new QLineEdit(&dialog);
    auto *speaker = new QLineEdit("Speaker", &dialog);
    auto *family = new QComboBox(&dialog);
    family->addItems(available);
    auto *language = new QComboBox(&dialog);
    language->addItems({"zh", "en", "ja", "yue", "ko"});
    form.addRow(QStringLiteral("项目名称"), name);
    form.addRow(QStringLiteral("说话人"), speaker);
    form.addRow(QStringLiteral("模型家族"), family);
    form.addRow(QStringLiteral("素材语言"), language);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form.addRow(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    const auto title = name->text().trimmed();
    const auto person = speaker->text().trimmed();
    require(!title.isEmpty() && !person.isEmpty() && !person.contains('|'),
            QStringLiteral("请填写有效的项目名称和说话人"));
    const auto id = newId();
    const auto path = directory(root_ + "/projects/" + id);
    saveProject({{"id", id}, {"name", title}, {"speaker", person}, {"language", language->currentText()},
        {"family", family->currentText()}, {"directory", path}});
    projects_->addItem(title, id);
    projects_->setCurrentIndex(projects_->count() - 1);
}

void TrainingPanel::saveProject(const QJsonObject &project)
{
    QSqlQuery query;
    query.prepare("INSERT INTO projects(id,name,state) VALUES(?,?,?) ON CONFLICT(id) DO UPDATE SET state=excluded.state");
    query.addBindValue(project.value("id").toString());
    query.addBindValue(project.value("name").toString());
    query.addBindValue(QJsonDocument(storedWorkspacePaths(project)).toJson(QJsonDocument::Compact));
    execute(query);
    if (projects_->currentData().toString() == project.value("id").toString())
        project_ = project;
}

void TrainingPanel::loadProject()
{
    QSqlQuery query;
    query.prepare("SELECT state FROM projects WHERE id=?");
    query.addBindValue(projects_->currentData().toString());
    execute(query);
    project_ = query.next() ? readWorkspaceRecord(query.value(0).toByteArray()) : QJsonObject{};
    query.finish();
    const auto pending = project_.value("pending").toString();
    if (!pending.isEmpty()) {
        query.prepare("SELECT state,result FROM jobs WHERE id=?");
        query.addBindValue(pending);
        execute(query);
        const bool exists = query.next();
        const auto status = exists ? query.value(0).toString() : QString();
        const auto result = exists ? readWorkspaceRecord(query.value(1).toByteArray()) : QJsonObject{};
        query.finish();
        if (status == "succeeded") {
            complete(pending, result, false);
        } else if (status != "queued" && status != "running") {
            project_.remove("pending");
            saveProject(project_);
        }
    }
    readClips();
    if (project_.contains("annotation"))
        loadLabels(project_.value("annotation").toString(), project_.value("annotation_automatic").toBool());
    reviewsDirty_ = false;
    const auto settings = project_.value("training_settings").toObject();
    selectDevice(settings.value("gpu").toString().toInt());
    batch_->setValue(settings.value("batch").toInt(1));
    epochs_->setValue(settings.value("epochs").toInt(4));
    status_->clear();
    showState();
}

void TrainingPanel::importAudio(bool clips)
{
    require(!project_.isEmpty(), QStringLiteral("请先创建训练项目"));
    require(!busy(), QStringLiteral("请等待当前阶段完成"));
    const auto sources = QFileDialog::getOpenFileNames(this, QStringLiteral("导入录音"), {},
        clips ? "WAV (*.wav)" : "Audio (*.wav *.flac *.mp3 *.m4a *.ogg)");
    if (sources.isEmpty())
        return;
    const QString folder = clips ? "clips" : "raw";
    const auto current = project_.value(folder).toString();
    const auto groupsKey = folder + "_batches";
    const auto originalKey = folder + "_original_names";
    const auto lastKey = "last_" + folder + "_batch";
    QString replace;
    if (!current.isEmpty()) {
        QMessageBox method(QMessageBox::Question, QStringLiteral("导入录音"),
            QStringLiteral("选择如何处理本次导入。"), QMessageBox::Cancel, this);
        auto *append = method.addButton(QStringLiteral("追加"), QMessageBox::AcceptRole);
        auto *overwrite = method.addButton(QStringLiteral("替换本批"), QMessageBox::DestructiveRole);
        method.button(QMessageBox::Cancel)->setText(QStringLiteral("取消"));
        method.setDefaultButton(append);
        method.exec();
        if (method.clickedButton() == overwrite)
            replace = project_.value(lastKey).toString("legacy");
        else if (method.clickedButton() != append)
            return;
    }
    const auto batch = newId();
    const auto groups = project_.value(groupsKey).toObject();
    const auto originalNames = project_.value(originalKey).toObject();
    const auto plan = recordingTask(this, QStringLiteral("正在比较录音内容…"), 0, [&](const auto &cancel, auto &) {
        return planRecordings(sources, current, groups, originalNames, replace, batch, cancel);
    });
    require(plan.error.isEmpty(), plan.error);
    QStringList details;
    if (!plan.duplicates.isEmpty())
        details.append(QStringLiteral("重复录音\n%1").arg(plan.duplicates.join('\n')));
    if (!plan.conflicts.isEmpty())
        details.append(QStringLiteral("同名但内容不同，将保留两份\n%1").arg(plan.conflicts.join('\n')));
    if (!plan.replaced.isEmpty())
        details.append(QStringLiteral("将从当前素材批次移除\n%1").arg(plan.replaced.join('\n')));
    QMessageBox preview(QMessageBox::Information, QStringLiteral("录音导入预览"),
        QStringLiteral("新增 %1 条，保留 %2 条，跳过重复 %3 条，同名异内容 %4 条，替换原批次 %5 条。")
            .arg(plan.additions).arg(plan.files.size() - plan.additions).arg(plan.duplicates.size())
            .arg(plan.conflicts.size()).arg(plan.replaced.size()),
        QMessageBox::Apply | QMessageBox::Cancel, this);
    preview.button(QMessageBox::Apply)->setText(QStringLiteral("应用"));
    preview.button(QMessageBox::Cancel)->setText(QStringLiteral("取消"));
    if (!details.isEmpty())
        preview.setDetailedText(details.join("\n\n"));
    if (plan.additions == 0 || preview.exec() != QMessageBox::Apply) {
        if (plan.additions == 0)
            status_->setText(QStringLiteral("本次没有新的录音，项目保持原样。"));
        return;
    }
    const auto projectDirectory = project_.value("directory").toString();
    const auto destination = nextArtifactPath(projectDirectory + "/" + folder, folder);
    const auto pendingDirectory = clips ? QString() : nextArtifactPath(projectDirectory + "/raw-pending", "raw");
    QStringList pending;
    for (const auto &name : project_.value("pending_recordings").toArray())
        pending.append(name.toString());
    if (!clips && !current.isEmpty() && !project_.contains("clips") && !project_.contains("pending_recordings")) {
        for (const auto &recording : plan.files) {
            if (!recording.incoming)
                pending.append(recording.name);
        }
    }
    const auto error = recordingTask(this, QStringLiteral("正在准备录音文件…"), int(plan.files.size()),
        [&](const auto &cancel, auto &completed) {
            return copyRecordings(plan, destination, pendingDirectory, pending, cancel, completed);
        });
    const auto rollback = [&] {
        QDir(destination).removeRecursively();
        if (!pendingDirectory.isEmpty())
            QDir(pendingDirectory).removeRecursively();
    };
    if (!error.isEmpty()) {
        rollback();
        require(false, error);
    }
    try {
        QJsonObject batches;
        QJsonObject names;
        QJsonObject ids;
        QJsonArray pendingNames;
        for (const auto &recording : plan.files) {
            batches[recording.name] = recording.batch;
            names[recording.name] = recording.originalName;
            if (clips)
                ids[recording.name] = recording.incoming ? newId() : clipId(recording.name);
            else if (recording.incoming || pending.contains(recording.name))
                pendingNames.append(recording.name);
        }
        auto state = project_;
        state["reviews"] = reviewDrafts();
        state[folder] = destination;
        state[groupsKey] = batches;
        state[originalKey] = names;
        state[lastKey] = batch;
        if (clips) {
            state["clip_ids"] = ids;
            state.remove("annotation");
        } else {
            state["raw_pending"] = pendingDirectory;
            state["pending_recordings"] = pendingNames;
            if (!replace.isEmpty()) {
                auto replaced = state.value("replace_sliced_batches").toArray();
                if (!replaced.contains(replace))
                    replaced.append(replace);
                state["replace_sliced_batches"] = replaced;
            }
        }
        invalidate(state);
        saveProject(state);
    } catch (...) {
        rollback();
        throw;
    }
    reviewsDirty_ = false;
    readClips();
    showState();
    status_->setText(QStringLiteral("已导入 %1 段录音").arg(plan.additions));
}

void TrainingPanel::readClips()
{
    const QSignalBlocker block(labels_);
    labels_->setRowCount(0);
    const auto path = project_.value("clips").toString();
    if (path.isEmpty())
        return;
    const auto clips = QDir(checkedPath(path)).entryInfoList({"*.wav"}, QDir::Files | QDir::NoSymLinks, QDir::Name);
    const auto reviewed = project_.value("reviews").toObject();
    for (const auto &clip : clips) {
        const auto row = labels_->rowCount();
        labels_->insertRow(row);
        auto *item = new QTableWidgetItem(clip.fileName());
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        item->setData(Qt::UserRole, checkedPath(clip.absoluteFilePath()));
        const auto id = clipId(clip.fileName());
        item->setData(Qt::UserRole + 1, id);
        labels_->setItem(row, 0, item);
        const auto draft = reviewed.value(id).toObject();
        labels_->setItem(row, 1, new QTableWidgetItem(draft.value("language").toString(project_.value("language").toString())));
        auto *status = new QTableWidgetItem(draft.value("status").toString(QStringLiteral("待校对")));
        status->setFlags(status->flags() & ~Qt::ItemIsEditable);
        labels_->setItem(row, 2, status);
        labels_->setItem(row, 3, new QTableWidgetItem(draft.value("text").toString()));
    }
    updateReviewFilter();
}

QString TrainingPanel::clipId(const QString &filename) const
{
    const auto id = project_.value("clip_ids").toObject().value(filename).toString();
    if (!id.isEmpty())
        return id;
    const auto anchor = project_.value("id").toString() + '/' + filename.toCaseFolded();
    return QString::fromLatin1(QCryptographicHash::hash(anchor.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QJsonObject TrainingPanel::reviewDrafts() const
{
    auto drafts = project_.value("reviews").toObject();
    for (int row = 0; row < labels_->rowCount(); ++row) {
        drafts[labels_->item(row, 0)->data(Qt::UserRole + 1).toString()] = QJsonObject{
            {"language", labels_->item(row, 1)->text()}, {"status", labels_->item(row, 2)->text()},
            {"text", labels_->item(row, 3)->text()}};
    }
    return drafts;
}

void TrainingPanel::updateReviewFilter()
{
    for (int row = 0; row < labels_->rowCount(); ++row) {
        labels_->setRowHidden(row, pendingOnly_->isChecked()
            && labels_->item(row, 2)->text() != QStringLiteral("待校对"));
    }
}

void TrainingPanel::saveAndAdvance()
{
    require(!busy(), QStringLiteral("请等待当前阶段完成"));
    const int row = ui::selectedRow(labels_);
    require(row >= 0, QStringLiteral("请选择要保存的片段"));
    const auto text = labels_->item(row, 3)->text().trimmed();
    const auto language = labels_->item(row, 1)->text().trimmed().toLower();
    const QStringList languages{"zh", "en", "ja", "yue", "ko"};
    require(!text.isEmpty() && !text.contains('|') && !text.contains('\n') && languages.contains(language),
        QStringLiteral("请填写有效的文本和语言。"));
    labels_->item(row, 2)->setText(QStringLiteral("已校对"));
    auto state = project_;
    state["reviews"] = reviewDrafts();
    saveProject(state);
    reviewsDirty_ = false;
    updateReviewFilter();
    for (int next = row + 1; next < labels_->rowCount(); ++next) {
        if (!labels_->isRowHidden(next)) {
            labels_->setCurrentCell(next, 0);
            labels_->scrollToItem(labels_->item(next, 0));
            break;
        }
    }
    labels_->setFocus(Qt::ShortcutFocusReason);
    status_->setText(QStringLiteral("已保存所选片段的校对。"));
}

void TrainingPanel::loadLabels(const QString &path, bool automatic)
{
    require(labels_->rowCount() > 0, QStringLiteral("请先导入或生成切片"));
    QFile file(checkedPath(path));
    require(file.open(QIODevice::ReadOnly), QStringLiteral("无法读取标注"));
    const bool imported = !automatic && path != project_.value("annotation").toString();
    QMap<QString, QList<int>> rows;
    const auto aliases = project_.value("clips_original_names").toObject();
    for (int row = 0; row < labels_->rowCount(); ++row) {
        const auto name = labels_->item(row, 0)->text();
        const auto original = aliases.value(name).toString(name);
        rows[name.toCaseFolded()].append(row);
        if (original.compare(name, Qt::CaseInsensitive) != 0)
            rows[original.toCaseFolded()].append(row);
    }
    QMap<int, QStringList> matches;
    QSet<int> repeated;
    QStringList missingAudio;
    QStringList malformed;
    QStringList ambiguous;
    const QStringList languages{"zh", "en", "ja", "yue", "ko"};
    const auto lines = QString::fromUtf8(file.readAll()).split('\n');
    for (qsizetype index = 0; index < lines.size(); ++index) {
        if (lines[index].trimmed().isEmpty())
            continue;
        auto fields = lines[index].trimmed().split('|');
        if (fields.size() != 4 || fields[0].isEmpty() || fields[1].isEmpty()
            || !languages.contains(fields[2].toLower()) || fields[3].trimmed().isEmpty()) {
            malformed.append(QString::number(index + 1));
            continue;
        }
        const auto clip = QDir::fromNativeSeparators(fields[0]);
        const auto filename = QFileInfo(clip).fileName();
        const auto candidates = rows.value(filename.toCaseFolded());
        int row = -1;
        for (const int candidate : candidates) {
            if (labels_->item(candidate, 0)->data(Qt::UserRole).toString().compare(clip, Qt::CaseInsensitive) == 0)
                row = candidate;
        }
        if (row < 0 && candidates.size() == 1)
            row = candidates.first();
        if (row < 0 && candidates.size() > 1 && imported) {
            QStringList names;
            for (const int candidate : candidates)
                names.append(labels_->item(candidate, 0)->text());
            bool selected = false;
            const auto name = QInputDialog::getItem(this, QStringLiteral("选择同名片段"),
                QStringLiteral("第 %1 行 · %2").arg(index + 1).arg(filename), names, 0, false, &selected);
            if (selected)
                row = candidates.at(names.indexOf(name));
        }
        if (row < 0) {
            auto &list = candidates.isEmpty() ? missingAudio : ambiguous;
            list.append(QStringLiteral("%1 · %2").arg(index + 1).arg(filename));
            continue;
        }
        if (repeated.contains(row) || matches.contains(row)) {
            repeated.insert(row);
            matches.remove(row);
            continue;
        }
        matches.insert(row, {fields[2].toLower(), fields[3]});
    }
    if (imported) {
        QStringList unlabelled;
        QStringList duplicateNames;
        QStringList changes;
        for (int row = 0; row < labels_->rowCount(); ++row) {
            const auto name = labels_->item(row, 0)->text();
            if (!matches.contains(row))
                unlabelled.append(name);
            else
                changes.append(QStringLiteral("%1 · %2").arg(name, matches.value(row)[1]));
            if (repeated.contains(row))
                duplicateNames.append(name);
        }
        QMessageBox preview(QMessageBox::Information, QStringLiteral("标注导入差异"),
            QStringLiteral("唯一匹配 %1 条，缺标片段 %2 条，音频缺失 %3 条，重复片段 %4 条，格式错误 %5 行，同名待确认 %6 条。")
                .arg(matches.size()).arg(unlabelled.size()).arg(missingAudio.size()).arg(repeated.size())
                .arg(malformed.size()).arg(ambiguous.size()),
            QMessageBox::Apply | QMessageBox::Cancel, this);
        preview.setDetailedText(QStringLiteral("将更新\n%1\n\n缺少标注\n%2\n\n找不到音频\n%3\n\n"
            "重复条目\n%4\n\n格式错误行号\n%5\n\n同名待确认\n%6")
            .arg(changes.join('\n'), unlabelled.join('\n'), missingAudio.join('\n'),
                 duplicateNames.join('\n'), malformed.join(", "), ambiguous.join('\n')));
        preview.button(QMessageBox::Apply)->setEnabled(!matches.isEmpty());
        preview.button(QMessageBox::Apply)->setText(QStringLiteral("应用"));
        preview.button(QMessageBox::Cancel)->setText(QStringLiteral("取消"));
        if (preview.exec() != QMessageBox::Apply)
            return;
    }
    const QSignalBlocker block(labels_);
    const auto drafts = project_.value("reviews").toObject();
    for (int row = 0; row < labels_->rowCount(); ++row) {
        const auto id = labels_->item(row, 0)->data(Qt::UserRole + 1).toString();
        if (!imported && drafts.contains(id) && (!automatic || !labels_->item(row, 3)->text().isEmpty()
                || labels_->item(row, 2)->text() == QStringLiteral("已排除")))
            continue;
        const auto found = matches.constFind(row);
        if (found != matches.cend()) {
            labels_->item(row, 1)->setText(found.value()[0]);
            labels_->item(row, 3)->setText(found.value()[1]);
            labels_->item(row, 2)->setText(imported || automatic ? QStringLiteral("待校对") : QStringLiteral("已校对"));
        } else if (!imported && !automatic && drafts.isEmpty() && !matches.isEmpty()) {
            labels_->item(row, 2)->setText(QStringLiteral("已排除"));
        }
    }
    if (imported)
        reviewsDirty_ = true;
    updateReviewFilter();
    status_->setText(QStringLiteral("已载入 %1 条标注，可试听校对。").arg(matches.size()));
}

void TrainingPanel::saveLabels()
{
    require(!busy() && labels_->rowCount() > 0, QStringLiteral("请准备切片或等待当前任务完成"));
    QStringList rows;
    const QStringList supported{"zh", "en", "ja", "yue", "ko"};
    for (int row = 0; row < labels_->rowCount(); ++row) {
        if (labels_->item(row, 2)->text() == QStringLiteral("已排除"))
            continue;
        const auto ref = checkedPath(labels_->item(row, 0)->data(Qt::UserRole).toString());
        const auto lang = labels_->item(row, 1)->text().trimmed().toLower();
        const auto text = labels_->item(row, 3)->text().trimmed();
        require(!text.isEmpty() && !text.contains('|') && !text.contains('\n') && supported.contains(lang),
                QStringLiteral("第 %1 条标注缺少文本、语言无效或含保留分隔符").arg(row + 1));
        rows.append(QStringList{ref, project_.value("speaker").toString(), lang, text}.join('|'));
    }
    require(!rows.isEmpty(), QStringLiteral("请保留至少一个校对片段。"));
    for (int row = 0; row < labels_->rowCount(); ++row) {
        if (labels_->item(row, 2)->text() != QStringLiteral("已排除"))
            labels_->item(row, 2)->setText(QStringLiteral("已校对"));
    }
    auto state = project_;
    state["reviews"] = reviewDrafts();
    const auto content = rows.join('\n').toUtf8() + '\n';
    const auto previous = project_.value("annotation").toString();
    if (!previous.isEmpty()) {
        QFile file(checkedPath(previous));
        if (file.open(QIODevice::ReadOnly) && file.readAll() == content) {
            saveProject(state);
            reviewsDirty_ = false;
            updateReviewFilter();
            return;
        }
    }
    const auto annotation = nextArtifactPath(project_.value("directory").toString(), "labels", ".list");
    QSaveFile out(annotation);
    require(out.open(QIODevice::WriteOnly) && out.write(content) == content.size() && out.commit(),
            QStringLiteral("无法保存校对结果"));
    invalidate(state);
    state["annotation"] = annotation;
    state.remove("annotation_automatic");
    saveProject(state);
    reviewsDirty_ = false;
    updateReviewFilter();
    showState();
}

void TrainingPanel::submit(const QString &operation, bool resume)
{
    require(!project_.isEmpty() && !busy(), QStringLiteral("请选择项目并等待当前任务结束"));
    const auto launcher = launcher_();
    require(!launcher.isEmpty(), QStringLiteral("请在「设置」中添加并选择引擎"));
    const auto training = operation == "train_sovits" || operation == "train_gpt";
    const auto device = operation == "slice" ? QString() : deviceIssue();
    require(device.isEmpty(), device);
    const auto installation = scheduler_.compatibility().inspect(launcher);
    const auto capability = resume ? "resume_" + operation.mid(6) : operation;
    const auto issue = installation.issue(capability, project_.value("family").toString(),
        project_.value("language") == "yue" ? "yue" : "zh");
    require(issue.isEmpty(), issue);
    if (training && labels_->rowCount() > 0)
        saveLabels();
    auto request = project_;
    request["operation"] = operation;
    request["project"] = project_.value("directory");
    request["gpu"] = gpu_->currentData().toString();
    request["batch"] = batch_->value();
    request["epochs"] = epochs_->value();
    request["source"] = project_.value(operation == "slice" ? "raw" : "clips");
    if (operation == "slice" && project_.contains("raw_pending")) {
        request["source"] = project_.value("raw_pending");
        request["seed_clips"] = project_.value("clips");
        const auto replaced = project_.value("replace_sliced_batches").toArray();
        const auto groups = project_.value("sliced_from_batches").toObject();
        QJsonArray discarded;
        if (!replaced.isEmpty()) {
            for (auto item = groups.constBegin(); item != groups.constEnd(); ++item) {
                if (replaced.contains(item.value()))
                    discarded.append(item.key());
            }
        }
        request["discarded_clips"] = discarded;
    }
    if (operation == "slice" || operation == "asr") {
        require(!request.value("source").toString().isEmpty(), QStringLiteral("请先导入该阶段所需的录音"));
        request["destination"] = nextArtifactPath(project_.value("directory").toString(), operation);
        request["asr_language"] = project_.value("language") == "yue" ? "yue" : "zh";
    } else if (operation == "prepare") {
        require(project_.contains("annotation"), QStringLiteral("请保存校对后的标注"));
        request["experiment"] = "ez_" + newId();
    } else {
        require(project_.contains("dataset") && project_.value("engine") == launcher,
                QStringLiteral("请先用当前引擎准备数据"));
    }
    if (resume) {
        const auto previous = resumeJob(operation);
        require(!previous.isEmpty(), QStringLiteral("没有与当前设置匹配的可读检查点，请重跑当前阶段"));
        request["resume_job"] = previous;
    }
    auto state = project_;
    if (operation == "slice" || operation == "asr")
        state["reviews"] = reviewDrafts();
    if (training) {
        state["training_settings"] = QJsonObject{{"gpu", request.value("gpu")}, {"batch", request.value("batch")},
                                                  {"epochs", request.value("epochs")}};
    }
    const auto task = scheduler_.enqueue(launcher, request, state);
    project_ = state;
    project_["pending"] = task;
    showState();
}

void TrainingPanel::complete(const QString &id, const QJsonObject &result, bool live)
{
    QSqlQuery query;
    query.prepare("SELECT launcher,operation,request FROM jobs WHERE id=?");
    query.addBindValue(id);
    execute(query);
    if (!query.next() || query.value(1) == "synthesize")
        return;
    const auto launcher = query.value(0).toString();
    const auto operation = query.value(1).toString();
    const auto request = readWorkspaceRecord(query.value(2).toByteArray());
    query.finish();
    query.prepare("SELECT state FROM projects WHERE id=?");
    query.addBindValue(request.value("id").toString());
    execute(query);
    if (!query.next())
        return;
    auto state = readWorkspaceRecord(query.value(0).toByteArray());
    query.finish();
    if (state.value("pending").toString() != id)
        return;
    state.remove("pending");
    if (operation == "slice") {
        auto ids = state.value("clip_ids").toObject();
        auto batches = state.value("sliced_from_batches").toObject();
        const auto sourceBatches = state.value("raw_batches").toObject();
        QSet<QString> names;
        for (const auto &item : result.value("clips").toArray())
            names.insert(QFileInfo(item.toString()).fileName());
        for (const auto &name : ids.keys()) {
            if (!names.contains(name))
                ids.remove(name);
        }
        for (const auto &name : batches.keys()) {
            if (!names.contains(name))
                batches.remove(name);
        }
        for (const auto &item : result.value("added_clips").toArray()) {
            const auto clip = item.toObject();
            const auto name = clip.value("name").toString();
            ids[name] = newId();
            batches[name] = sourceBatches.value(clip.value("recording").toString()).toString("legacy");
        }
        state["clip_ids"] = ids;
        state["sliced_from_batches"] = batches;
        state["clips"] = request.value("destination");
        state.remove("raw_pending");
        state.remove("pending_recordings");
        state.remove("replace_sliced_batches");
        state.remove("annotation");
        invalidate(state);
    } else if (operation == "asr") {
        state["annotation"] = result.value("annotation");
        state["annotation_automatic"] = true;
        state["experiment"] = "ez_" + newId();
        invalidate(state);
    } else if (operation == "prepare") {
        invalidate(state);
        state.remove("training_recovery");
        state["experiment"] = request.value("experiment");
        state["dataset"] = result.value("dataset");
        const int sampleCount = result.value("sample_count").toInt();
        if (sampleCount > 0)
            state["sampleCount"] = sampleCount;
        state["engine"] = resolvedEngineLauncher(launcher);
    } else if (operation == "train_sovits" || operation == "train_gpt") {
        state[operation == "train_sovits" ? "sovits" : "gpt"] = result.value("weight");
    }
    saveProject(state);
    const bool current = project_.value("id") == state.value("id");
    if (live && current && operation == "slice")
        readClips();
    if (live && current && operation == "asr") {
        loadLabels(state.value("annotation").toString(), true);
        state["reviews"] = reviewDrafts();
        saveProject(state);
    }
    if (live && operation == "train_sovits"
        && scheduler_.compatibility().inspect(launcher).issue("train_gpt", request.value("family").toString()).isEmpty()) {
        auto next = request;
        next["operation"] = "train_gpt";
        next.remove("resume_job");
        next.remove("compat_fingerprint");
        state["pending"] = scheduler_.enqueue(launcher, next, state);
        if (current)
            project_ = state;
    }
    showState();
}

void TrainingPanel::importVoice()
{
    const int row = ui::selectedRow(labels_);
    const auto *clip = labels_->item(row, 0);
    require(project_.contains("sovits") && project_.contains("gpt") && clip,
            QStringLiteral("请完成两阶段训练，并选中一个参考片段"));
    const QMap<QString, QString> language{{"zh", QStringLiteral("中文")}, {"en", QStringLiteral("英文")},
        {"ja", QStringLiteral("日文")}, {"ko", QStringLiteral("韩文")}, {"yue", QStringLiteral("粤语")}};
    auto values = project_;
    values["reference"] = clip->data(Qt::UserRole).toString();
    values["prompt"] = labels_->item(row, 3)->text();
    values["prompt_language"] = language.value(labels_->item(row, 1)->text());
    openVoice_(values);
}

bool TrainingPanel::busy() const
{
    if (!project_.contains("pending"))
        return false;
    QSqlQuery query;
    query.prepare("SELECT 1 FROM jobs WHERE id=? AND state IN ('queued','running')");
    query.addBindValue(project_.value("pending").toString());
    return query.exec() && query.next();
}

bool TrainingPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress && (watched == labels_ || watched == labels_->viewport())) {
        const auto &key = *static_cast<QKeyEvent *>(event);
        if (key.key() == Qt::Key_Space && key.modifiers() == Qt::NoModifier) {
            playClip_->click();
            return true;
        }
        if (advanceReview(key)) {
            saveNext_->click();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

QString TrainingPanel::nextTrainingStage() const
{
    return project_.value("sovits").toString().isEmpty() ? "train_sovits" : "train_gpt";
}

QString TrainingPanel::resumeJob(const QString &operation) const
{
    const auto launcher = launcher_();
    if (launcher.isEmpty() || project_.value("engine") != launcher
        || project_.value("dataset").toString().isEmpty() || project_.contains("gpt"))
        return {};
    QSqlQuery query;
    query.prepare("SELECT id,state,request FROM jobs WHERE launcher=? AND operation=? ORDER BY rowid DESC");
    query.addBindValue(launcher);
    query.addBindValue(operation);
    if (!query.exec())
        return {};
    const QRegularExpression jobId("^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$");
    const QRegularExpression runId("^ez_[0-9a-f]{32}$");
    while (query.next()) {
        const auto request = readWorkspaceRecord(query.value(2).toByteArray());
        if (request.value("id") != project_.value("id") || request.value("experiment") != project_.value("experiment"))
            continue;
        const auto id = query.value(0).toString();
        const auto state = query.value(1).toString();
        if (state == "succeeded" || state == "queued" || state == "running")
            return {};
        if (!jobId.match(id).hasMatch() || request.value("gpu").toString() != gpu_->currentData().toString()
            || request.value("batch").toInt() != batch_->value() || request.value("epochs").toInt() != epochs_->value()
            || request.value("annotation") != project_.value("annotation"))
            continue;
        try {
            QFile record(checkedPath(root_ + "/jobs/" + id + "/training-run.json"));
            if (!record.open(QIODevice::ReadOnly) || record.size() > 8 * 1024 * 1024)
                continue;
            const auto metadata = QJsonDocument::fromJson(record.readAll()).object();
            if (!metadata.value("resume_signature").isObject())
                continue;
            const auto logs = engineRoot(launcher) + "/logs";
            const QFileInfo run(checkedPath(metadata.value("checkpoints").toString()));
            if (run.absolutePath().compare(logs, Qt::CaseInsensitive) != 0 || !runId.match(run.fileName()).hasMatch())
                continue;
            const auto directory = run.absoluteFilePath();
            if (operation == "train_sovits") {
                if (readableTrainingCheckpoint(directory + "/logs_s2_v5turbo_lora_32/G_233333333333.pth"))
                    return id;
            } else {
                const auto folder = checkedPath(directory + "/logs_s1_v5turbo/ckpt");
                const auto files = QDir(folder).entryInfoList({"epoch=*-step=*.ckpt"}, QDir::Files);
                for (const auto &checkpoint : files) {
                    if (readableTrainingCheckpoint(checkpoint.absoluteFilePath()))
                        return id;
                }
            }
        } catch (const std::exception &) {
            // An inaccessible attempt cannot supply a checkpoint.
        }
    }
    return {};
}

void TrainingPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!reviewsDirty_ && project_.contains("engine")
        && resolvedEngineLauncher(project_.value("engine").toString()) != project_.value("engine").toString()) {
        try {
            loadProject();
        } catch (const std::exception &error) {
            status_->setText(QString::fromUtf8(error.what()));
        }
    } else {
        showState();
    }
}

void TrainingPanel::showState()
{
    const auto active = busy();
    const auto launcher = launcher_();
    const auto installation = scheduler_.compatibility().inspect(launcher);
    const auto family = project_.value("family").toString();
    const auto language = project_.value("language") == "yue" ? "yue" : "zh";
    const auto trainingStage = nextTrainingStage();
    const auto training = installation.issue(trainingStage, family);
    const auto resumeIssue = installation.issue("resume_" + trainingStage.mid(6), family);
    const auto device = deviceIssue();
    const auto resumeJobId = !active && resumeIssue.isEmpty() && device.isEmpty() ? resumeJob(trainingStage) : QString();
    const auto canResume = !resumeJobId.isEmpty();
    resume_->setVisible(canResume);
    const bool canCreate = !installation.families(fullTraining).isEmpty();
    create_->setEnabled(canCreate);
    const auto setAvailable = [this, active](QPushButton *button, const QString &issue, bool hasInputs) {
        button->setEnabled(!active && !project_.isEmpty() && hasInputs && issue.isEmpty());
        button->setToolTip(issue);
    };
    const auto slice = installation.issue("slice");
    const auto asr = installation.issue("asr", {}, language);
    const auto prepare = installation.issue("prepare", family);
    const bool hasRaw = project_.contains("raw") || project_.contains("raw_pending");
    const bool hasClips = project_.contains("clips");
    const bool hasAnnotation = project_.contains("annotation");
    const bool hasDataset = project_.contains("dataset");
    const bool hasWeights = project_.contains("gpt") && project_.contains("sovits");
    train_->setVisible(!hasWeights);
    preview_->setVisible(hasWeights);
    const bool sameEngine = project_.value("engine").toString().compare(launcher, Qt::CaseInsensitive) == 0;
    const auto trainingIssue = hasDataset && !sameEngine
        ? QStringLiteral("此项目的数据已在另一整合包中准备。可切回原整合包，或为当前整合包重新准备数据。") : training;
    setAvailable(slice_, slice, hasRaw);
    setAvailable(transcribe_, asr.isEmpty() ? device : asr, hasClips);
    setAvailable(prepare_, prepare.isEmpty() ? device : prepare, hasClips);
    setAvailable(train_, trainingIssue.isEmpty() ? device : trainingIssue, hasDataset && !hasWeights);
    setAvailable(preview_, {}, hasWeights);
    gpu_->setToolTip(device.isEmpty() ? gpu_->currentText() : device);
    QString issue;
    if (project_.isEmpty()) {
        if (!canCreate) {
            issue = QStringLiteral("此整合包没有可完成全部训练阶段的模型家族。");
            if (!installation.error.isEmpty() || installation.package().isEmpty())
                issue += "\n" + installation.description();
        }
    } else if (!active && !hasWeights) {
        if (project_.contains("raw_pending") || (hasRaw && !hasClips))
            issue = slice;
        else if (hasDataset)
            issue = trainingIssue;
        else if (hasAnnotation)
            issue = prepare;
        else if (hasClips && !asr.isEmpty())
            issue = asr + QStringLiteral("\n可以导入标注或手工校对。");
        else if (!hasClips && !prepare.isEmpty())
            issue = prepare;
        if (!issue.isEmpty() && !family.isEmpty())
            issue = QStringLiteral("项目家族 %1 · %2").arg(family, issue);
    }
    compatibilityStatus_->setText(issue);
    compatibilityStatus_->setVisible(!issue.isEmpty());
    compatibilityActions_->setVisible(!issue.isEmpty());
    const auto advise = [this](QPushButton *action, const QString &message) {
        stage_->setText(message);
        if (action == suggested_)
            return;
        const auto highlight = [](QPushButton *button, bool selected) {
            if (!button)
                return;
            button->setObjectName(selected ? QStringLiteral("primary") : QString());
            button->style()->unpolish(button);
            button->style()->polish(button);
            button->update();
        };
        highlight(suggested_, false);
        suggested_ = action;
        highlight(suggested_, true);
    };
    if (project_.isEmpty()) {
        advise(create_, QStringLiteral("新建项目后即可导入录音或切片。"));
    } else if (active) {
        advise(nullptr, QStringLiteral("已排队，等待运行。"));
    } else if (project_.contains("raw_pending")) {
        advise(slice_, QStringLiteral("有新录音待切片。已有校对会保留。"));
    } else if (project_.contains("gpt") && project_.contains("sovits")) {
        advise(preview_, QStringLiteral("权重已保存。选择参考片段后可保存声音。"));
    } else if (hasClips && !device.isEmpty()) {
        advise(nullptr, device);
    } else if (canResume) {
        advise(resume_, QStringLiteral("可以从检查点继续训练。"));
    } else if (project_.contains("sovits")) {
        advise(train_, QStringLiteral("SoVITS 已训练完成。下一阶段是 GPT。"));
    } else if (project_.contains("dataset")) {
        const int count = project_.value("sampleCount").toInt();
        advise(train_, count > 0 ? QStringLiteral("%1 条训练样本已就绪。").arg(count)
                                 : QStringLiteral("训练数据已就绪。"));
    } else if (project_.contains("annotation")) {
        advise(prepare_, project_.value("training_recovery").toString(
            QStringLiteral("双击文本校对。「准备数据」会自动保存标注。")));
    } else if (project_.contains("clips")) {
        advise(transcribe_, QStringLiteral("转写、导入标注，或双击文本手动填写。"));
    } else if (project_.contains("raw")) {
        advise(slice_, QStringLiteral("录音已导入，可以切片。"));
    } else {
        advise(recordings_, QStringLiteral("导入整段录音或已切好的 WAV。"));
    }
    updateStageVisibility();
}

void TrainingPanel::refreshEngineSelection()
{
    try {
        showState();
    } catch (const std::exception &error) {
        status_->setText(QString::fromUtf8(error.what()));
    }
}

void TrainingPanel::setDeviceNames(const QMap<int, QString> &devices)
{
    if (deviceNames_ == devices)
        return;
    const auto selected = gpu_->currentData().toInt();
    deviceNames_ = devices;
    selectDevice(selected);
    refreshEngineSelection();
}

QString TrainingPanel::deviceIssue() const
{
    if (!deviceNames_.isEmpty() && !deviceNames_.contains(gpu_->currentData().toInt()))
        return QStringLiteral("原训练设备不可用，请选择当前可用的设备。");
    return {};
}

void TrainingPanel::selectDevice(int ordinal)
{
    const QSignalBlocker block(gpu_);
    gpu_->clear();
    const auto names = deviceNames_.values();
    for (auto item = deviceNames_.cbegin(); item != deviceNames_.cend(); ++item) {
        const auto name = names.count(item.value()) > 1
            ? QStringLiteral("%1 · 设备 %2").arg(item.value()).arg(item.key()) : item.value();
        gpu_->addItem(name, item.key());
    }
    auto selected = gpu_->findData(ordinal);
    if (selected < 0) {
        const auto label = !deviceNames_.isEmpty() ? QStringLiteral("原设备不可用")
            : ordinal == 0 ? QStringLiteral("引擎默认设备") : QStringLiteral("待确认设备");
        gpu_->addItem(label, ordinal);
        selected = gpu_->count() - 1;
    }
    gpu_->setCurrentIndex(selected);
}

void TrainingPanel::updateStageVisibility()
{
    const auto &activity = scheduler_.activity();
    stage_->setVisible(activity.id.isEmpty() || project_.value("pending").toString() != activity.id);
}

}
