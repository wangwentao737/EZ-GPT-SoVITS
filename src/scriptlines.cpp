#include "scriptlines.h"
#include "engine.h"
#include "jobs.h"
#include "settings.h"
#include "ui.h"
#include "waveexport.h"
#include "workspacepaths.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMap>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <algorithm>
#include <stdexcept>

namespace ez {
namespace {

QString id()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

struct ScriptLine {
    QString key;
    QString revision;
    QString voice;
    QString text;
};

struct LineResult {
    QString state;
    QString caption;
    QString audio;
    QString digest;
};

struct Voice {
    QString name;
    QJsonObject snapshot;
};

void execute(QSqlQuery &query)
{
    if (!query.exec())
        throw std::runtime_error(query.lastError().text().toStdString());
}

class ScriptDialog final : public QDialog {
public:
    ScriptDialog(QWidget *parent, TaskScheduler &scheduler, QString launcher, QString voice,
                 const QStringList &append, std::function<void(const QString &)> play);

private:
    void save(const QList<ScriptLine> &lines);
    void replaceLines(QList<ScriptLine> lines);
    void fill();
    void updateResults();
    void addLine();
    void removeRows();
    void assignVoice();
    void moveLine(int by);
    void submitRows(const QList<int> &rows);
    void exportRows(bool merge);
    QList<int> selection() const;
    QList<int> allRows() const;
    QString voiceName(const ScriptLine &line) const;
    LineResult resultFor(QSqlQuery &query, const ScriptLine &line);

    TaskScheduler &scheduler_;
    QString launcher_;
    QString voice_;
    std::function<void(const QString &)> play_;
    QList<ScriptLine> lines_;
    QList<LineResult> results_;
    QMap<QString, Voice> voices_;
    QTableWidget *table_;
    QLabel *message_;
};

ScriptDialog::ScriptDialog(QWidget *parent, TaskScheduler &scheduler, QString launcher, QString voice,
                           const QStringList &append, std::function<void(const QString &)> play)
    : QDialog(parent), scheduler_(scheduler), launcher_(std::move(launcher)), voice_(std::move(voice)), play_(std::move(play))
{
    setWindowTitle(QStringLiteral("台词表"));
    resize(930, 590);
    QSqlQuery presets;
    presets.prepare("SELECT id,name,settings FROM voices ORDER BY name");
    execute(presets);
    while (presets.next())
        voices_.insert(presets.value(0).toString(), {presets.value(1).toString(), readWorkspaceRecord(presets.value(2).toByteArray())});
    const auto saved = QJsonDocument::fromJson(workspaceSetting("scriptLines").toUtf8()).array();
    for (const auto &item : saved) {
        const auto object = item.toObject();
        if (!object.value("id").toString().isEmpty()) {
            lines_.append({object.value("id").toString(), object.value("revision").toString(),
                object.value("voice").toString(), object.value("text").toString()});
        }
    }
    for (const auto &line : append)
        lines_.append({id(), id(), voice_, line});
    if (!append.isEmpty())
        save(lines_);

    auto *layout = new QVBoxLayout(this);
    table_ = new QTableWidget(this);
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels({QStringLiteral("顺序"), QStringLiteral("声音"), QStringLiteral("台词"), QStringLiteral("状态")});
    ui::styleTable(table_);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    layout->addWidget(table_, 1);
    auto *edit = new QHBoxLayout;
    auto *generate = new QHBoxLayout;
    const auto button = [this](QHBoxLayout *row, const QString &name, auto action) {
        auto *control = new QPushButton(name, this);
        row->addWidget(control);
        connect(control, &QPushButton::clicked, this, [this, action] {
            try {
                action();
            } catch (const std::exception &error) {
                message_->setText(QString::fromUtf8(error.what()));
            }
        });
        return control;
    };
    button(edit, QStringLiteral("添加一行"), [this] { addLine(); });
    button(edit, QStringLiteral("移除所选"), [this] { removeRows(); });
    button(edit, QStringLiteral("给所选行指定声音"), [this] { assignVoice(); });
    button(edit, QStringLiteral("上移"), [this] { moveLine(-1); });
    button(edit, QStringLiteral("下移"), [this] { moveLine(1); });
    edit->addStretch();
    layout->addLayout(edit);
    auto *submit = button(generate, QStringLiteral("提交整表"), [this] { submitRows(allRows()); });
    submit->setObjectName("primary");
    button(generate, QStringLiteral("只重做所选"), [this] { submitRows(selection()); });
    button(generate, QStringLiteral("试听所选行"), [this] {
        const auto rows = selection();
        if (rows.size() == 1 && !results_.at(rows.first()).audio.isEmpty())
            play_(results_.at(rows.first()).audio);
    });
    button(generate, QStringLiteral("按顺序导出"), [this] { exportRows(false); });
    button(generate, QStringLiteral("合并 WAV"), [this] { exportRows(true); });
    generate->addStretch();
    layout->addLayout(generate);
    message_ = ui::hint({}, this);
    layout->addWidget(message_);
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(close);
    connect(table_, &QTableWidget::itemChanged, this, [this](const QTableWidgetItem *item) {
        if (item->column() != 2 || lines_[item->row()].text == item->text())
            return;
        const auto prior = lines_[item->row()];
        auto &line = lines_[item->row()];
        line.text = item->text();
        line.revision = id();
        try {
            save(lines_);
            updateResults();
        } catch (const std::exception &error) {
            line = prior;
            fill();
            message_->setText(QString::fromUtf8(error.what()));
        }
    });
    connect(&scheduler_, &TaskScheduler::changed, this, [this] {
        try {
            updateResults();
        } catch (const std::exception &error) {
            message_->setText(QString::fromUtf8(error.what()));
        }
    });
    fill();
}

void ScriptDialog::save(const QList<ScriptLine> &lines)
{
    QJsonArray stored;
    for (const auto &line : lines)
        stored.append(QJsonObject{{"id", line.key}, {"revision", line.revision}, {"voice", line.voice}, {"text", line.text}});
    setWorkspaceSetting("scriptLines", QString::fromUtf8(QJsonDocument(stored).toJson(QJsonDocument::Compact)));
}

void ScriptDialog::replaceLines(QList<ScriptLine> lines)
{
    save(lines);
    lines_ = std::move(lines);
    fill();
}

QString ScriptDialog::voiceName(const ScriptLine &line) const
{
    return voices_.contains(line.voice) ? voices_.value(line.voice).name : QStringLiteral("待选择声音");
}

void ScriptDialog::fill()
{
    const QSignalBlocker block(table_);
    table_->setRowCount(lines_.size());
    for (int row = 0; row < lines_.size(); ++row) {
        const auto &line = lines_.at(row);
        const QStringList cells{QString::number(row + 1), voiceName(line), line.text, {}};
        for (qsizetype column = 0; column < cells.size(); ++column) {
            auto *cell = new QTableWidgetItem(cells.at(column));
            if (column != 2)
                cell->setFlags(cell->flags() & ~Qt::ItemIsEditable);
            table_->setItem(row, column, cell);
        }
    }
    updateResults();
}

LineResult ScriptDialog::resultFor(QSqlQuery &query, const ScriptLine &line)
{
    query.bindValue(0, line.key);
    execute(query);
    if (!query.next())
        return {{}, QStringLiteral("待生成"), {}, {}};
    if (query.value(0).toString() != line.revision)
        return {{}, QStringLiteral("修改后待重做"), {}, {}};
    const auto state = query.value(1).toString();
    const auto request = QJsonDocument::fromJson(query.value(3).toByteArray()).object();
    const auto result = readWorkspaceResult(query.value(2).toByteArray(), request);
    const auto audio = result.value("audio").toString();
    static const QMap<QString, QString> captions{{"running", QStringLiteral("正在生成")}, {"queued", QStringLiteral("排队")},
        {"succeeded", QStringLiteral("已生成")}, {"failed", QStringLiteral("失败")}, {"interrupted", QStringLiteral("中断")},
        {"cancelled", QStringLiteral("已取消")}};
    const bool ready = state == "succeeded" && QFileInfo(audio).isFile();
    return {state, state == "succeeded" && !ready ? QStringLiteral("成品缺失") : captions.value(state, state),
        ready ? audio : QString(), result.value("sha256").toString()};
}

void ScriptDialog::updateResults()
{
    QSqlQuery query;
    query.prepare("SELECT json_extract(CAST(request AS TEXT),'$.line_revision'),state,result,request FROM jobs "
        "WHERE operation='synthesize' AND json_valid(CAST(request AS TEXT)) "
        "AND json_extract(CAST(request AS TEXT),'$.line_id')=? ORDER BY rowid DESC LIMIT 1");
    const QSignalBlocker block(table_);
    results_.clear();
    for (int row = 0; row < lines_.size(); ++row) {
        const auto result = resultFor(query, lines_.at(row));
        results_.append(result);
        table_->item(row, 3)->setText(result.caption);
        table_->item(row, 3)->setToolTip(result.audio);
        query.finish();
    }
}

QList<int> ScriptDialog::selection() const
{
    QList<int> rows;
    for (const auto &selection : table_->selectionModel()->selectedRows())
        rows.append(selection.row());
    std::sort(rows.begin(), rows.end());
    return rows;
}

QList<int> ScriptDialog::allRows() const
{
    QList<int> rows;
    for (int row = 0; row < lines_.size(); ++row)
        rows.append(row);
    return rows;
}

void ScriptDialog::addLine()
{
    auto lines = lines_;
    lines.append({id(), id(), voice_, {}});
    replaceLines(std::move(lines));
    const auto row = table_->rowCount() - 1;
    table_->setCurrentCell(row, 2);
    table_->editItem(table_->item(row, 2));
}

void ScriptDialog::removeRows()
{
    const auto rows = selection();
    auto lines = lines_;
    for (auto row = rows.crbegin(); row != rows.crend(); ++row)
        lines.removeAt(*row);
    replaceLines(std::move(lines));
}

void ScriptDialog::assignVoice()
{
    const auto rows = selection();
    if (rows.isEmpty() || voices_.isEmpty())
        return;
    QDialog picker(this);
    picker.setWindowTitle(QStringLiteral("给所选行指定声音"));
    QVBoxLayout layout(&picker);
    QComboBox voices(&picker);
    for (auto item = voices_.constBegin(); item != voices_.constEnd(); ++item)
        voices.addItem(item.value().name, item.key());
    voices.setCurrentIndex(qMax(0, voices.findData(lines_.at(rows.first()).voice)));
    layout.addWidget(&voices);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &picker);
    layout.addWidget(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &picker, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &picker, &QDialog::reject);
    if (picker.exec() != QDialog::Accepted)
        return;
    const auto voice = voices.currentData().toString();
    auto lines = lines_;
    for (const int row : rows) {
        auto &line = lines[row];
        if (line.voice != voice) {
            line.voice = voice;
            line.revision = id();
        }
    }
    replaceLines(std::move(lines));
}

void ScriptDialog::moveLine(int by)
{
    const auto rows = selection();
    if (rows.size() != 1) {
        message_->setText(QStringLiteral("请选择一行移动。"));
        return;
    }
    const int row = rows.first();
    const int target = row + by;
    if (target < 0 || target >= lines_.size())
        return;
    auto lines = lines_;
    lines.move(row, target);
    replaceLines(std::move(lines));
    table_->selectRow(target);
}

void ScriptDialog::submitRows(const QList<int> &rows)
{
    if (launcher_.isEmpty() || rows.isEmpty()) {
        message_->setText(launcher_.isEmpty() ? QStringLiteral("请先在设置选择引擎。") : QStringLiteral("请选择需要生成的台词。"));
        return;
    }
    QList<QJsonObject> snapshots;
    for (const int row : rows) {
        const auto &line = lines_.at(row);
        if (line.text.trimmed().isEmpty() || !voices_.contains(line.voice)) {
            message_->setText(QStringLiteral("请填写第 %1 行台词并选择声音。").arg(row + 1));
            return;
        }
        auto snapshot = voices_.value(line.voice).snapshot;
        snapshot["operation"] = "synthesize";
        snapshot["text"] = line.text.trimmed();
        snapshot["voice_id"] = line.voice;
        snapshot["name"] = voices_.value(line.voice).name;
        snapshot["language"] = snapshot.value("default_language").toString(QStringLiteral("中文"));
        snapshot["line_id"] = line.key;
        snapshot["line_revision"] = line.revision;
        snapshot["line_order"] = row + 1;
        snapshots.append(snapshot);
    }
    scheduler_.enqueueSynthesisBatch(launcher_, snapshots);
    message_->setText(QStringLiteral("已提交 %1 条台词。可在任务记录查看批次。").arg(snapshots.size()));
}

void ScriptDialog::exportRows(bool merge)
{
    updateResults();
    QStringList incomplete;
    QList<int> pending;
    QList<WaveExportItem> items;
    for (int row = 0; row < lines_.size(); ++row) {
        const auto &result = results_.at(row);
        if (result.audio.isEmpty()) {
            incomplete.append(QStringLiteral("第 %1 行 · %2").arg(row + 1).arg(result.caption));
            if (result.state != "queued" && result.state != "running")
                pending.append(row);
            continue;
        }
        auto role = voiceName(lines_.at(row)).trimmed();
        role.replace(QRegularExpression(R"([<>:"/\\|?*\x00-\x1F])"), "_");
        while (role.endsWith('.') || role.endsWith(' '))
            role.chop(1);
        const auto filename = QStringLiteral("%1_%2.wav").arg(row + 1, qMax(3, int(QString::number(lines_.size()).size())),
            10, QChar('0')).arg(role.isEmpty() ? QStringLiteral("声音") : role.left(80));
        items.append({result.audio, result.digest, filename});
    }
    if (!incomplete.isEmpty()) {
        QMessageBox confirm(QMessageBox::Question, QStringLiteral("部分台词尚无成品"),
            QStringLiteral("%1 条台词尚未生成当前修订的 WAV。").arg(incomplete.size()), QMessageBox::Cancel, this);
        confirm.setDetailedText(incomplete.join('\n'));
        auto *exportReady = confirm.addButton(QStringLiteral("只导出成功项"), QMessageBox::AcceptRole);
        exportReady->setEnabled(!items.isEmpty());
        auto *fillMissing = confirm.addButton(QStringLiteral("补齐未完成项"), QMessageBox::ActionRole);
        fillMissing->setEnabled(!pending.isEmpty());
        confirm.exec();
        if (confirm.clickedButton() == fillMissing) {
            submitRows(pending);
            return;
        }
        if (confirm.clickedButton() != exportReady)
            return;
    }
    if (items.isEmpty()) {
        message_->setText(QStringLiteral("台词表还没有可导出的成品。"));
        return;
    }
    QString target;
    QSet<QString> overwrite;
    int gap = 0;
    if (merge) {
        bool accepted = false;
        gap = QInputDialog::getInt(this, QStringLiteral("合并 WAV"), QStringLiteral("句间停顿（毫秒）"),
            250, 0, 10000, 10, &accepted);
        if (!accepted)
            return;
        target = QFileDialog::getSaveFileName(this, QStringLiteral("保存合并 WAV"), "script.wav", "WAV (*.wav)",
            nullptr, QFileDialog::DontConfirmOverwrite);
        if (!target.isEmpty() && !target.endsWith(".wav", Qt::CaseInsensitive))
            target += ".wav";
        if (QFileInfo(target).isFile())
            overwrite.insert(QFileInfo(target).fileName());
    } else {
        target = QFileDialog::getExistingDirectory(this, QStringLiteral("选择顺序导出目录"));
        if (!target.isEmpty()) {
            for (const auto &item : items) {
                if (QFileInfo::exists(QDir(target).filePath(item.filename)))
                    overwrite.insert(item.filename);
            }
        }
    }
    if (target.isEmpty())
        return;
    if (!overwrite.isEmpty() && QMessageBox::question(this, QStringLiteral("确认覆盖"),
            QStringLiteral("目标位置存在同名文件。是否覆盖 %1 个文件？").arg(overwrite.size())) != QMessageBox::Yes)
        return;
    std::atomic_bool cancelled = false;
    QProgressDialog progress(QStringLiteral("正在校验并导出 WAV…"), QStringLiteral("取消"), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    QFutureWatcher<QString> watcher;
    QEventLoop wait;
    connect(&progress, &QProgressDialog::canceled, &wait, [&] { cancelled.store(true); });
    connect(&watcher, &QFutureWatcherBase::finished, &wait, &QEventLoop::quit);
    const auto future = QtConcurrent::run([&] {
        return merge ? exportMergedWave(items, target, gap, !overwrite.isEmpty(), cancelled)
            : exportOrderedWaves(items, target, overwrite, cancelled);
    });
    watcher.setFuture(future);
    if (!future.isFinished()) {
        progress.show();
        wait.exec();
    }
    const auto error = future.result();
    message_->setText(error.isEmpty() ? QStringLiteral("已导出 %1 条台词。").arg(items.size()) : error);
}

}

void editScriptLines(QWidget *parent, TaskScheduler &scheduler, const QString &launcher,
                     const QString &voice, const QStringList &append,
                     const std::function<void(const QString &)> &play)
{
    ScriptDialog dialog(parent, scheduler, launcher, voice, append, play);
    dialog.exec();
}

}
