#include "training.h"
#include "engine.h"
#include "jobs.h"
#include "resources.h"

#include <QAudioOutput>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QShowEvent>
#include <QSpinBox>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableWidget>
#include <QUuid>
#include <stdexcept>
#include <utility>

namespace ez {
namespace {

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
    for (const auto &key : {"dataset", "sampleCount", "gpt", "sovits", "engine"})
        project.remove(key);
}

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
    auto *selection = new QHBoxLayout;
    projects_ = new QComboBox(this);
    projects_->setPlaceholderText(QStringLiteral("创建或选择训练项目"));
    selection->addWidget(projects_, 1);
    layout->addLayout(selection);
    stage_ = new QLabel(this);
    stage_->setWordWrap(true);
    layout->addWidget(stage_);
    status_ = new QLabel(this);
    status_->setWordWrap(true);
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
    button(selection, QStringLiteral("新建项目"), [this] { addProject(); });
    auto *files = new QHBoxLayout;
    button(files, QStringLiteral("导入原始录音"), [this] { importAudio(false); });
    button(files, QStringLiteral("切片"), [this] { submit("slice"); });
    button(files, QStringLiteral("导入已切片录音"), [this] { importAudio(true); });
    button(files, QStringLiteral("中文自动转写"), [this] { submit("asr"); });
    button(files, QStringLiteral("导入标注"), [this] {
        require(!busy(), QStringLiteral("请等待当前阶段完成"));
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("导入已有标注"), {}, "List (*.list *.txt)");
        if (!path.isEmpty())
            loadLabels(path);
    });
    layout->addLayout(files);

    labels_ = new QTableWidget(0, 3, this);
    labels_->setHorizontalHeaderLabels({QStringLiteral("片段"), QStringLiteral("语言"), QStringLiteral("文本")});
    labels_->setSelectionBehavior(QAbstractItemView::SelectRows);
    labels_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    labels_->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(labels_, 1);
    player_ = new QMediaPlayer(this);
    player_->setAudioOutput(new QAudioOutput(player_));
    auto *labelsActions = new QHBoxLayout;
    button(labelsActions, QStringLiteral("试听片段"), [this] {
        const auto *item = labels_->item(labels_->currentRow(), 0);
        if (item) {
            player_->setSource(QUrl::fromLocalFile(checkedPath(item->data(Qt::UserRole).toString())));
            player_->play();
        }
    });
    button(labelsActions, QStringLiteral("排除片段"), [this] {
        require(!busy(), QStringLiteral("请等待当前阶段完成"));
        labels_->removeRow(labels_->currentRow());
    });
    button(labelsActions, QStringLiteral("保存标注"), [this] { saveLabels(); });
    button(labelsActions, QStringLiteral("准备数据"), [this] {
        saveLabels();
        submit("prepare");
    });
    labelsActions->addStretch();
    layout->addLayout(labelsActions);

    auto *training = new QHBoxLayout;
    gpu_ = new QSpinBox(this);
    gpu_->setPrefix(QStringLiteral("显卡 "));
    gpu_->setRange(0, 15);
    training->addWidget(gpu_);
    batch_ = new QSpinBox(this);
    batch_->setPrefix(QStringLiteral("批量 "));
    batch_->setRange(1, 40);
    training->addWidget(batch_);
    epochs_ = new QSpinBox(this);
    epochs_->setPrefix(QStringLiteral("轮数 "));
    epochs_->setRange(2, 50);
    epochs_->setValue(4);
    training->addWidget(epochs_);
    button(training, QStringLiteral("开始训练"), [this] {
        submit(nextTrainingStage());
    });
    resume_ = button(training, QStringLiteral("从检查点继续"), [this] {
        submit(nextTrainingStage(), true);
    });
    resume_->setVisible(false);
    for (auto *control : {gpu_, batch_, epochs_})
        connect(control, &QSpinBox::valueChanged, this, [this] { showState(); });
    button(training, QStringLiteral("试听并入库"), [this] { importVoice(); });
    training->addStretch();
    layout->addLayout(training);
    layout->addWidget(status_);

    connect(projects_, &QComboBox::currentIndexChanged, this, [this] {
        try {
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
    auto *name = new QLineEdit(&dialog);
    auto *speaker = new QLineEdit("Speaker", &dialog);
    auto *family = new QComboBox(&dialog);
    family->addItems({"v5turbo", "v5dev", "v2Pro", "v2ProPlus", "v2"});
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
    query.addBindValue(QJsonDocument(project).toJson(QJsonDocument::Compact));
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
    project_ = query.next() ? QJsonDocument::fromJson(query.value(0).toByteArray()).object() : QJsonObject{};
    query.finish();
    const auto pending = project_.value("pending").toString();
    if (!pending.isEmpty()) {
        query.prepare("SELECT state,result FROM jobs WHERE id=?");
        query.addBindValue(pending);
        execute(query);
        const bool exists = query.next();
        const auto status = exists ? query.value(0).toString() : QString();
        const auto result = exists ? QJsonDocument::fromJson(query.value(1).toByteArray()).object() : QJsonObject{};
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
        loadLabels(project_.value("annotation").toString());
    const auto settings = project_.value("training_settings").toObject();
    if (!settings.isEmpty()) {
        gpu_->setValue(settings.value("gpu").toString().toInt());
        batch_->setValue(settings.value("batch").toInt(1));
        epochs_->setValue(settings.value("epochs").toInt(4));
    }
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
    const auto folder = clips ? "clips" : "raw";
    const auto destination = directory(project_.value("directory").toString() + "/" + folder + "/" + newId());
    int count = 0;
    for (const auto &source : sources) {
        const auto input = checkedPath(source);
        const QFileInfo file(input);
        const auto filename = clips ? file.fileName() : "clip-" + QString::number(count) + "." + file.suffix().toLower();
        const auto output = checkedPath(destination + "/" + filename);
        require(QFile::copy(input, output), QStringLiteral("无法复制录音：%1").arg(input));
        ++count;
    }
    project_[folder] = destination;
    if (!clips)
        project_.remove("clips");
    project_.remove("annotation");
    invalidate(project_);
    saveProject(project_);
    readClips();
    showState();
    status_->setText(QStringLiteral("已导入 %1 段录音").arg(count));
}

void TrainingPanel::readClips()
{
    labels_->setRowCount(0);
    const auto path = project_.value("clips").toString();
    if (path.isEmpty())
        return;
    const auto clips = QDir(checkedPath(path)).entryInfoList({"*.wav"}, QDir::Files, QDir::Name);
    for (const auto &clip : clips) {
        const auto row = labels_->rowCount();
        labels_->insertRow(row);
        auto *item = new QTableWidgetItem(clip.fileName());
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        item->setData(Qt::UserRole, checkedPath(clip.absoluteFilePath()));
        labels_->setItem(row, 0, item);
        labels_->setItem(row, 1, new QTableWidgetItem(project_.value("language").toString()));
        labels_->setItem(row, 2, new QTableWidgetItem);
    }
}

void TrainingPanel::loadLabels(const QString &path)
{
    require(labels_->rowCount() > 0, QStringLiteral("请先导入或生成切片"));
    QFile file(checkedPath(path));
    require(file.open(QIODevice::ReadOnly), QStringLiteral("无法读取标注"));
    QMap<QString, int> rows;
    for (int row = 0; row < labels_->rowCount(); ++row)
        rows.insert(labels_->item(row, 0)->text(), row);
    QList<int> matched;
    const auto lines = QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
    for (const auto &line : lines) {
        const auto fields = line.trimmed().split('|');
        if (fields.size() != 4)
            continue;
        const auto filename = QFileInfo(QDir::fromNativeSeparators(fields[0])).fileName();
        if (auto row = rows.constFind(filename); row != rows.cend()) {
            labels_->item(row.value(), 1)->setText(fields[2].toLower());
            labels_->item(row.value(), 2)->setText(fields[3]);
            matched.append(row.value());
        }
    }
    require(!matched.isEmpty(), QStringLiteral("标注中的文件名与项目切片不匹配"));
    if (path == project_.value("annotation").toString()) {
        for (int row = labels_->rowCount() - 1; row >= 0; --row) {
            if (!matched.contains(row))
                labels_->removeRow(row);
        }
    }
    status_->setText(QStringLiteral("已载入 %1 条标注，请试听校对").arg(matched.size()));
}

void TrainingPanel::saveLabels()
{
    require(!busy() && labels_->rowCount() > 0, QStringLiteral("请准备切片或等待当前任务完成"));
    QStringList rows;
    const QStringList supported{"zh", "en", "ja", "yue", "ko"};
    for (int row = 0; row < labels_->rowCount(); ++row) {
        const auto ref = checkedPath(labels_->item(row, 0)->data(Qt::UserRole).toString());
        const auto lang = labels_->item(row, 1)->text().trimmed().toLower();
        const auto text = labels_->item(row, 2)->text().trimmed();
        require(!text.isEmpty() && !text.contains('|') && !text.contains('\n') && supported.contains(lang),
                QStringLiteral("第 %1 条标注缺少文本、语言无效或含保留分隔符").arg(row + 1));
        rows.append(QStringList{ref, project_.value("speaker").toString(), lang, text}.join('|'));
    }
    const auto content = rows.join('\n').toUtf8() + '\n';
    const auto previous = project_.value("annotation").toString();
    if (!previous.isEmpty()) {
        QFile file(checkedPath(previous));
        if (file.open(QIODevice::ReadOnly) && file.readAll() == content)
            return;
    }
    const auto annotation = checkedPath(project_.value("directory").toString() + "/labels-" + newId() + ".list");
    QSaveFile out(annotation);
    require(out.open(QIODevice::WriteOnly) && out.write(content) == content.size() && out.commit(),
            QStringLiteral("无法保存校对结果"));
    project_["annotation"] = annotation;
    project_["experiment"] = "ez_" + newId();
    invalidate(project_);
    saveProject(project_);
    showState();
}

void TrainingPanel::submit(const QString &operation, bool resume)
{
    require(!project_.isEmpty() && !busy(), QStringLiteral("请选择项目并等待当前任务结束"));
    const auto launcher = launcher_();
    require(!launcher.isEmpty(), QStringLiteral("请在「设置」中添加并选择引擎"));
    const auto training = operation == "train_sovits" || operation == "train_gpt";
    if (training && labels_->rowCount() > 0)
        saveLabels();
    auto request = project_;
    request["operation"] = operation;
    request["project"] = project_.value("directory");
    request["gpu"] = QString::number(gpu_->value());
    request["batch"] = batch_->value();
    request["epochs"] = epochs_->value();
    request["source"] = project_.value(operation == "slice" ? "raw" : "clips");
    if (operation == "slice" || operation == "asr") {
        require(!request.value("source").toString().isEmpty(), QStringLiteral("请先导入该阶段所需的录音"));
        request["destination"] = project_.value("directory").toString() + "/" + operation + "-" + newId();
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
    const auto request = QJsonDocument::fromJson(query.value(2).toByteArray()).object();
    query.finish();
    query.prepare("SELECT state FROM projects WHERE id=?");
    query.addBindValue(request.value("id").toString());
    execute(query);
    if (!query.next())
        return;
    auto state = QJsonDocument::fromJson(query.value(0).toByteArray()).object();
    query.finish();
    if (state.value("pending").toString() != id)
        return;
    state.remove("pending");
    if (operation == "slice") {
        state["clips"] = request.value("destination");
        state.remove("annotation");
        invalidate(state);
    } else if (operation == "asr") {
        state["annotation"] = result.value("annotation");
        state["experiment"] = "ez_" + newId();
        invalidate(state);
    } else if (operation == "prepare") {
        invalidate(state);
        state["experiment"] = request.value("experiment");
        state["dataset"] = result.value("dataset");
        const int sampleCount = result.value("sample_count").toInt();
        if (sampleCount > 0)
            state["sampleCount"] = sampleCount;
        state["engine"] = launcher;
    } else if (operation == "train_sovits" || operation == "train_gpt") {
        state[operation == "train_sovits" ? "sovits" : "gpt"] = result.value("weight");
    }
    saveProject(state);
    const bool current = project_.value("id") == state.value("id");
    if (live && current && operation == "slice")
        readClips();
    if (live && current && operation == "asr")
        loadLabels(state.value("annotation").toString());
    if (live && operation == "train_sovits") {
        auto next = request;
        next["operation"] = "train_gpt";
        next.remove("resume_job");
        state["pending"] = scheduler_.enqueue(launcher, next, state);
        if (current)
            project_ = state;
    }
    showState();
}

void TrainingPanel::importVoice()
{
    const auto *clip = labels_->item(labels_->currentRow(), 0);
    require(project_.contains("sovits") && project_.contains("gpt") && clip,
            QStringLiteral("请完成两阶段训练，并选中一个参考片段"));
    const QMap<QString, QString> language{{"zh", QStringLiteral("中文")}, {"en", QStringLiteral("英文")},
        {"ja", QStringLiteral("日文")}, {"ko", QStringLiteral("韩文")}, {"yue", QStringLiteral("粤语")}};
    auto values = project_;
    values["reference"] = clip->data(Qt::UserRole).toString();
    values["prompt"] = labels_->item(labels_->currentRow(), 2)->text();
    values["prompt_language"] = language.value(labels_->item(labels_->currentRow(), 1)->text());
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

QString TrainingPanel::nextTrainingStage() const
{
    return project_.value("sovits").toString().isEmpty() ? "train_sovits" : "train_gpt";
}

QString TrainingPanel::resumeJob(const QString &operation) const
{
    const auto launcher = launcher_();
    if (launcher.isEmpty() || project_.value("family") != "v5turbo" || project_.value("engine") != launcher
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
        const auto request = QJsonDocument::fromJson(query.value(2).toByteArray()).object();
        if (request.value("id") != project_.value("id") || request.value("experiment") != project_.value("experiment"))
            continue;
        const auto id = query.value(0).toString();
        const auto state = query.value(1).toString();
        if (state == "succeeded" || state == "queued" || state == "running")
            return {};
        if (!jobId.match(id).hasMatch() || request.value("gpu").toString() != QString::number(gpu_->value())
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
    showState();
}

void TrainingPanel::showState()
{
    resume_->setVisible(!busy() && !resumeJob(nextTrainingStage()).isEmpty());
    if (project_.isEmpty()) {
        stage_->setText(QStringLiteral("新建项目后，导入录音或已切好的 WAV 片段。"));
        return;
    }
    if (busy()) {
        stage_->setText(QStringLiteral("当前阶段已提交。进度和错误详情见任务记录。"));
        return;
    }
    if (project_.contains("gpt") && project_.contains("sovits"))
        stage_->setText(QStringLiteral("模型已训练完成。选择参考片段，点击「试听并入库」。"));
    else if (project_.contains("sovits"))
        stage_->setText(QStringLiteral("SoVITS 阶段已完成。点击「开始训练」继续 GPT 阶段。"));
    else if (project_.contains("dataset")) {
        const int count = project_.value("sampleCount").toInt();
        stage_->setText(count > 0 ? QStringLiteral("%1 条训练样本已校验。设置批量和轮数，再点击「开始训练」。").arg(count)
                                  : QStringLiteral("训练数据已就绪。设置批量和轮数，再点击「开始训练」。"));
    }
    else if (project_.contains("clips"))
        stage_->setText(QStringLiteral("切片已就绪。校对每条标注，保存后准备训练数据。"));
    else
        stage_->setText(QStringLiteral("导入原始录音进行切片，或直接导入已切好的 WAV 片段。"));
}

}
