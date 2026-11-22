#include "library.h"
#include "engine.h"
#include "jobs.h"
#include "settings.h"
#include "synthesis.h"
#include "ui.h"

#include <QAction>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTabWidget>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <stdexcept>
#include <utility>

namespace ez {
namespace {

enum Kind { Preset, Gpt, SoVits, Reference, Recording, Clip, Annotation, Output };
const QStringList typeNames{QStringLiteral("声音预设"), QStringLiteral("GPT 权重"), QStringLiteral("SoVITS 权重"),
    QStringLiteral("参考音频"), QStringLiteral("原始录音"), QStringLiteral("切片"), QStringLiteral("标注"),
    QStringLiteral("合成成品")};

struct Asset {
    Kind kind;
    QString name;
    QString path;
    QString family;
    QSet<QString> voices;
    QSet<QString> projects;
    QSet<QString> jobs;
    bool managedCopy = false;
};

struct Voice {
    QString name;
    QJsonObject settings;
    QString verifiedEngine;
};

struct Library {
    QMap<QString, Voice> voices;
    QMap<QString, QString> projects;
    QMap<QString, QString> jobs;
    QMap<QString, Asset> files;
    QString error;
};

[[noreturn]] void fail(const QString &message)
{
    throw std::runtime_error(message.toStdString());
}

void check(bool ok, const auto &query)
{
    if (!ok)
        fail(query.lastError().text());
}

QString pathKey(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
}

bool isWithin(const QString &path, const QString &directory)
{
    return pathKey(path).startsWith(pathKey(directory) + '/');
}

QJsonObject readObject(const QVariant &value)
{
    const auto document = QJsonDocument::fromJson(value.toByteArray());
    if (!document.isObject())
        fail(QStringLiteral("素材库中有无法读取的引用记录。"));
    return document.object();
}

void addFile(Library &library, Kind kind, const QString &path, const QString &family = {},
             const QString &voice = {}, const QString &project = {}, const QString &job = {},
             const QString &displayName = {})
{
    if (path.isEmpty())
        return;
    const auto key = pathKey(path);
    auto item = library.files.find(key);
    if (item == library.files.end())
        item = library.files.insert(key, {kind, QFileInfo(path).fileName(), QDir::fromNativeSeparators(path),
            family, {}, {}, {}, false});
    auto &file = item.value();
    if (!displayName.isEmpty())
        file.name = displayName;
    if (!voice.isEmpty())
        file.voices.insert(voice);
    if (!project.isEmpty())
        file.projects.insert(project);
    if (!job.isEmpty())
        file.jobs.insert(job);
}

void addInputs(Library &library, const QJsonObject &settings, const QString &voice,
               const QString &project, const QString &job = {})
{
    const auto family = settings.value("family").toString();
    for (const auto &[key, kind] : {std::pair{"gpt", Gpt}, {"sovits", SoVits}, {"reference", Reference}}) {
        addFile(library, kind, settings.value(key).toString(), kind == Reference ? QString() : family, voice, project, job,
                settings.value(QString::fromLatin1(key) + "_name").toString());
    }
}

QFileInfoList filesIn(const QString &directory, const QString &parent, const QStringList &patterns)
{
    if (!isWithin(directory, parent))
        return {};
    try {
        return QDir(checkedPath(directory)).entryInfoList(patterns, QDir::Files | QDir::NoSymLinks);
    } catch (const std::exception &) {
        // Disconnected legacy paths and reparse points are not scanned.
        return {};
    }
}

Library readLibrary(QSqlDatabase database, const QString &root, bool scan)
{
    Library library;
    QSqlQuery query(database);
    check(query.exec("SELECT id,name,state FROM projects ORDER BY name"), query);
    while (query.next()) {
        const auto id = query.value(0).toString();
        const auto state = readObject(query.value(2));
        library.projects.insert(id, query.value(1).toString());
        addInputs(library, state, {}, id);
        addFile(library, Annotation, state.value("annotation").toString(), {}, {}, id);
        if (!scan)
            continue;
        for (const auto &[key, kind] : {std::pair{"raw", Recording}, {"clips", Clip}}) {
            const auto path = state.value(key).toString();
            for (const auto &file : filesIn(path, root + "/projects",
                    {"*.wav", "*.flac", "*.mp3", "*.m4a", "*.ogg"})) {
                addFile(library, kind, file.absoluteFilePath(), {}, {}, id);
            }
        }
    }

    check(query.exec("SELECT id,name,settings,verified_engine FROM voices ORDER BY name"), query);
    while (query.next()) {
        const auto id = query.value(0).toString();
        const auto settings = readObject(query.value(2));
        library.voices.insert(id, {query.value(1).toString(), settings, query.value(3).toString()});
        addInputs(library, settings, id, settings.value("project_id").toString());
    }

    check(query.exec("SELECT id,created,operation,request,result FROM jobs ORDER BY rowid"), query);
    while (query.next()) {
        const auto id = query.value(0).toString();
        const auto operation = query.value(2).toString();
        const auto request = readObject(query.value(3));
        const auto voice = request.value("voice_id").toString();
        const auto project = request.value("project_id").toString(request.value("id").toString());
        const auto family = request.value("family").toString();
        library.jobs.insert(id, query.value(1).toString() + " · " + id.left(8));
        addInputs(library, request, voice, project, id);
        addFile(library, Annotation, request.value("annotation").toString(), {}, {}, project, id);
        if (query.value(4).toByteArray().isEmpty())
            continue;
        const auto result = readObject(query.value(4));
        addFile(library, Output, result.value("audio").toString(), family, voice, project, id);
        addFile(library, operation == "train_gpt" ? Gpt : SoVits, result.value("weight").toString(), family, {}, project, id);
        addFile(library, Annotation, result.value("annotation").toString(), {}, {}, project, id);
        for (const auto &clip : result.value("clips").toArray())
            addFile(library, Clip, clip.toString(), {}, {}, project, id);
    }

    if (!scan)
        return library;
    const auto assets = root + "/assets";
    const QRegularExpression digestFile("^[a-f0-9]{64}\\.[a-z0-9]+$", QRegularExpression::CaseInsensitiveOption);
    const auto scanCopies = [&](const QString &folder, const QStringList &patterns, Kind kind,
                                const QString &family = QString()) {
        for (const auto &file : filesIn(folder, assets, patterns)) {
            const auto path = file.absoluteFilePath();
            addFile(library, kind, path, family);
            library.files[pathKey(path)].managedCopy = digestFile.match(file.fileName()).hasMatch();
        }
    };
    scanCopies(assets + "/audio", {"*.wav", "*.flac", "*.mp3"}, Reference);
    for (const auto &family : QStringList{"v5turbo", "v5dev", "v2", "v2Pro", "v2ProPlus"}) {
        scanCopies(assets + "/models/" + family + "/GPT", {"*.ckpt"}, Gpt, family);
        scanCopies(assets + "/models/" + family + "/SoVITS", {"*.pth"}, SoVits, family);
    }
    return library;
}

Library loadLibrary(const QString &root)
{
    const auto connection = QUuid::createUuid().toString();
    Library result;
    {
        auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setConnectOptions("QSQLITE_OPEN_READONLY");
        try {
            database.setDatabaseName(checkedPath(root + "/library.sqlite"));
            check(database.open(), database);
            check(database.transaction(), database);
            result = readLibrary(database, root, true);
            check(database.commit(), database);
        } catch (const std::exception &error) {
            result.error = QString::fromUtf8(error.what());
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}

class LibraryPage final : public QWidget {
public:
    LibraryPage(QString root, const QString &defaultOutput, TaskScheduler &scheduler, SynthesisPanel *synthesis,
                QTabWidget *tabs);

private:
    void showEvent(QShowEvent *event) override;
    void reload();
    void refill();
    void showDetails();
    void renameVoice();
    void removeCopy();
    QString selection() const;
    QString associated(const Asset &asset) const;

    QString root_;
    SynthesisPanel *synthesis_;
    Library library_;
    QFutureWatcher<Library> watcher_;
    bool loading_ = false;
    bool reloadPending_ = false;
    QComboBox *kind_;
    QComboBox *voice_;
    QComboBox *project_;
    QTableWidget *table_;
    QLabel *empty_;
    QPlainTextEdit *details_;
    QLabel *status_;
    QAction *useVoice_;
    QPushButton *rename_;
    QPushButton *remove_;
    QPushButton *play_;
    QPushButton *folder_;
};

LibraryPage::LibraryPage(QString root, const QString &defaultOutput, TaskScheduler &scheduler,
                         SynthesisPanel *synthesis, QTabWidget *tabs)
    : QWidget(tabs), root_(std::move(root)), synthesis_(synthesis)
{
    auto *layout = new QVBoxLayout(this);
    ui::pageHeader(layout, this, QStringLiteral("声音与素材"),
        QStringLiteral("左键选中一项可查看详情。右键可使用声音、播放音频或打开文件位置。"));
    auto *filters = new QHBoxLayout;
    kind_ = new QComboBox(this);
    kind_->addItem(QStringLiteral("全部类型"), -1);
    for (int kind = 0; kind < typeNames.size(); ++kind)
        kind_->addItem(typeNames[kind], kind);
    voice_ = new QComboBox(this);
    voice_->addItem(QStringLiteral("全部声音"));
    project_ = new QComboBox(this);
    project_->addItem(QStringLiteral("全部项目"));
    kind_->setAccessibleName(QStringLiteral("按素材类型筛选"));
    voice_->setAccessibleName(QStringLiteral("按声音筛选"));
    project_->setAccessibleName(QStringLiteral("按项目筛选"));
    filters->addWidget(kind_);
    filters->addWidget(project_, 1);
    filters->addWidget(voice_, 1);
    layout->addLayout(filters);

    table_ = new QTableWidget(0, 4, this);
    table_->setHorizontalHeaderLabels({QStringLiteral("名称"), QStringLiteral("类型"),
        QStringLiteral("模型家族"), QStringLiteral("关联")});
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui::styleTable(table_);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(table_, 1);
    empty_ = new QLabel(QStringLiteral("导入声音，或在训练完成后将新声音保存到这里。"), this);
    empty_->setObjectName("emptyState");
    empty_->setWordWrap(true);
    empty_->setAlignment(Qt::AlignCenter);
    layout->addWidget(empty_, 1);
    table_->hide();
    details_ = new QPlainTextEdit(this);
    details_->setReadOnly(true);
    details_->setPlaceholderText(QStringLiteral("选择一项可查看文件和关联记录。"));
    details_->setMaximumHeight(120);
    layout->addWidget(details_);

    auto *actions = new QHBoxLayout;
    auto *importVoice = new QPushButton(QStringLiteral("导入声音"), this);
    importVoice->setObjectName("primary");
    useVoice_ = new QAction(QStringLiteral("使用此声音"), this);
    rename_ = new QPushButton(QStringLiteral("重命名声音"), this);
    play_ = new QPushButton(QStringLiteral("播放"), this);
    folder_ = new QPushButton(QStringLiteral("所在目录"), this);
    remove_ = new QPushButton(QStringLiteral("移除素材"), this);
    auto *folders = new QPushButton(QStringLiteral("打开分类目录"), this);
    for (auto *button : {importVoice, rename_, play_, folder_, remove_, folders})
        actions->addWidget(button);
    actions->addStretch();
    layout->addLayout(actions);
    status_ = ui::hint({}, this);
    layout->addWidget(status_);

    for (auto *box : {kind_, voice_, project_})
        connect(box, &QComboBox::currentIndexChanged, this, [this] { refill(); });
    connect(table_, &QTableWidget::itemSelectionChanged, this, &LibraryPage::showDetails);
    connect(&scheduler, &TaskScheduler::changed, this, [this] {
        if (isVisible())
            reload();
    });
    connect(&watcher_, &QFutureWatcher<Library>::finished, this, [this] {
        loading_ = false;
        auto next = watcher_.result();
        status_->setText(next.error);
        if (next.error.isEmpty()) {
            library_ = std::move(next);
            const auto populate = [](QComboBox *box, const QString &label, const auto &names, auto display) {
                const QSignalBlocker block(box);
                const auto previous = box->currentData();
                box->clear();
                box->addItem(label);
                for (auto item = names.cbegin(); item != names.cend(); ++item)
                    box->addItem(display(item.value()), item.key());
                box->setCurrentIndex(qMax(0, box->findData(previous)));
            };
            populate(project_, QStringLiteral("全部项目"), library_.projects, [](const QString &name) { return name; });
            populate(voice_, QStringLiteral("全部声音"), library_.voices, [](const Voice &voice) { return voice.name; });
            refill();
        }
        if (std::exchange(reloadPending_, false))
            reload();
    });
    connect(importVoice, &QPushButton::clicked, this, [=] {
        tabs->setCurrentWidget(synthesis);
        synthesis->importVoice();
    });
    connect(useVoice_, &QAction::triggered, this, [this, tabs] {
        const auto id = selection().mid(6);
        synthesis_->refreshVoices(id);
        tabs->setCurrentWidget(synthesis_);
    });
    connect(rename_, &QPushButton::clicked, this, &LibraryPage::renameVoice);
    connect(remove_, &QPushButton::clicked, this, &LibraryPage::removeCopy);
    connect(play_, &QPushButton::clicked, this, [=, this] {
        const auto file = library_.files.constFind(selection());
        if (file == library_.files.cend())
            return;
        try {
            synthesis->playAudio(file->path);
            tabs->setCurrentWidget(synthesis);
        } catch (const std::exception &error) {
            status_->setText(QString::fromUtf8(error.what()));
        }
    });
    const auto open = [this](const QString &directory) {
        try {
            const auto path = checkedPath(directory);
            if (!QFileInfo(path).isDir() || !QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
                fail(QStringLiteral("无法打开目录：%1").arg(path));
        } catch (const std::exception &error) {
            status_->setText(QString::fromUtf8(error.what()));
        }
    };
    connect(folder_, &QPushButton::clicked, this, [this, open] {
        if (const auto file = library_.files.constFind(selection()); file != library_.files.cend())
            open(QFileInfo(file->path).absolutePath());
    });
    auto *menu = new QMenu(folders);
    const QMap<QString, QString> categories{{QStringLiteral("模型目录"), "/assets/models"},
        {QStringLiteral("参考音频目录"), "/assets/audio"}, {QStringLiteral("成品目录"), "/outputs"},
        {QStringLiteral("训练项目目录"), "/projects"}};
    for (auto item = categories.cbegin(); item != categories.cend(); ++item) {
        const auto relative = item.value();
        menu->addAction(item.key(), this, [this, defaultOutput, open, relative] {
            try {
                const auto path = relative == "/outputs" ? configuredOutputRoot(defaultOutput) : checkedPath(root_ + relative);
                if (!QDir().mkpath(path))
                    fail(QStringLiteral("无法创建目录：%1").arg(path));
                open(path);
            } catch (const std::exception &error) {
                status_->setText(QString::fromUtf8(error.what()));
            }
        });
    }
    folders->setMenu(menu);
    ui::enableRowMenu(table_, {rename_, play_, folder_, remove_}, {useVoice_});
    showDetails();
}

void LibraryPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    reload();
}

void LibraryPage::reload()
{
    if (loading_) {
        reloadPending_ = true;
        return;
    }
    loading_ = true;
    watcher_.setFuture(QtConcurrent::run([root = root_] { return loadLibrary(root); }));
}

QString LibraryPage::selection() const
{
    const auto *item = table_->item(ui::selectedRow(table_), 0);
    return item ? item->data(Qt::UserRole).toString() : QString();
}

QString LibraryPage::associated(const Asset &asset) const
{
    QStringList refs;
    for (const auto &id : asset.voices) {
        const auto voice = library_.voices.constFind(id);
        refs << QStringLiteral("声音 · %1").arg(voice == library_.voices.cend() ? id : voice->name);
    }
    for (const auto &id : asset.projects)
        refs << QStringLiteral("项目 · %1").arg(library_.projects.value(id, id));
    refs.sort(Qt::CaseInsensitive);
    if (!asset.jobs.isEmpty())
        refs << QStringLiteral("%1 条任务记录").arg(asset.jobs.size());
    return refs.join(" · ");
}

void LibraryPage::refill()
{
    const auto selected = selection();
    const int type = kind_->currentData().toInt();
    const auto voice = voice_->currentData().toString();
    const auto project = project_->currentData().toString();
    const auto append = [this, selected](const QString &key, const QStringList &fields) {
        const int row = table_->rowCount();
        table_->insertRow(row);
        for (int column = 0; column < fields.size(); ++column)
            table_->setItem(row, column, new QTableWidgetItem(fields[column]));
        table_->item(row, 0)->setToolTip(fields.constFirst());
        table_->item(row, 0)->setData(Qt::UserRole, key);
        if (key == selected)
            table_->selectRow(row);
    };
    table_->setUpdatesEnabled(false);
    {
        const QSignalBlocker block(table_);
        table_->setRowCount(0);
        if (type == -1 || type == Preset) {
            for (auto item = library_.voices.cbegin(); item != library_.voices.cend(); ++item) {
                const auto &preset = item.value();
                const auto projectId = preset.settings.value("project_id").toString();
                if ((!voice.isEmpty() && item.key() != voice) || (!project.isEmpty() && projectId != project))
                    continue;
                append("voice:" + item.key(), {preset.name, typeNames[Preset], preset.settings.value("family").toString(),
                    library_.projects.value(projectId, preset.verifiedEngine.isEmpty()
                        ? QStringLiteral("待验证") : QStringLiteral("已验证"))});
            }
        }
        for (auto item = library_.files.cbegin(); item != library_.files.cend(); ++item) {
            const auto &file = item.value();
            if ((type != -1 && file.kind != type) || (!voice.isEmpty() && !file.voices.contains(voice))
                || (!project.isEmpty() && !file.projects.contains(project)))
                continue;
            append(item.key(), {file.name, typeNames[file.kind], file.family, associated(file)});
        }
    }
    table_->setUpdatesEnabled(true);
    const bool empty = table_->rowCount() == 0;
    table_->setVisible(!empty);
    empty_->setVisible(empty);
    empty_->setText(library_.voices.isEmpty() && library_.files.isEmpty()
        ? QStringLiteral("导入声音，或在训练完成后将新声音保存到这里。")
        : QStringLiteral("当前筛选没有结果，可更改类型、项目或声音。"));
    showDetails();
}

void LibraryPage::showDetails()
{
    const auto selected = selection();
    const auto preset = selected.startsWith("voice:") ? library_.voices.constFind(selected.mid(6)) : library_.voices.cend();
    const auto file = library_.files.constFind(selected);
    useVoice_->setEnabled(preset != library_.voices.cend());
    rename_->setEnabled(useVoice_->isEnabled());
    remove_->setEnabled(file != library_.files.cend() && file->managedCopy);
    play_->setEnabled(file != library_.files.cend()
                      && (file->kind == Reference || file->kind == Recording || file->kind == Clip || file->kind == Output));
    folder_->setEnabled(file != library_.files.cend());
    if (preset != library_.voices.cend()) {
        const auto &settings = preset->settings;
        details_->setPlainText(QStringLiteral("GPT 权重 · %1\nSoVITS 权重 · %2\n参考音频 · %3\n参考文本 · %4\n参考语言 · %5\n默认台词语言 · %6\n验证引擎 · %7")
            .arg(settings.value("gpt").toString(), settings.value("sovits").toString(),
                 settings.value("reference").toString(), settings.value("prompt").toString(),
                 settings.value("prompt_language").toString(), settings.value("default_language").toString(QStringLiteral("中文")),
                 preset->verifiedEngine.isEmpty()
                    ? QStringLiteral("待验证") : preset->verifiedEngine));
    } else if (file != library_.files.cend()) {
        QStringList description{QDir::toNativeSeparators(file->path), associated(*file)};
        for (const auto &id : file->jobs)
            description << QStringLiteral("任务 · %1").arg(library_.jobs.value(id, id));
        details_->setPlainText(description.join('\n').trimmed());
    } else {
        details_->clear();
    }
}

void LibraryPage::renameVoice()
{
    const auto id = selection().mid(6);
    const auto voice = library_.voices.value(id);
    bool accepted = false;
    const auto name = QInputDialog::getText(this, QStringLiteral("重命名声音"), QStringLiteral("声音名称"),
        QLineEdit::Normal, voice.name, &accepted).trimmed();
    if (!accepted || name.isEmpty() || name == voice.name)
        return;
    try {
        auto settings = voice.settings;
        settings["name"] = name;
        QSqlQuery query;
        query.prepare("UPDATE voices SET name=?,settings=? WHERE id=?");
        query.addBindValue(name);
        query.addBindValue(QJsonDocument(settings).toJson(QJsonDocument::Compact));
        query.addBindValue(id);
        check(query.exec(), query);
        synthesis_->refreshVoices();
        reload();
    } catch (const std::exception &error) {
        status_->setText(QString::fromUtf8(error.what()));
    }
}

void LibraryPage::removeCopy()
{
    const auto key = selection();
    const auto file = library_.files.value(key);
    if (!file.managedCopy)
        return;
    const auto referenced = [](const Asset &asset) {
        return !asset.voices.isEmpty() || !asset.projects.isEmpty() || !asset.jobs.isEmpty();
    };
    if (referenced(file)) {
        QMessageBox::information(this, QStringLiteral("素材正在使用"),
                                 QStringLiteral("此素材关联 %1。可在下方详情查看引用记录。").arg(associated(file)));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("移除素材"),
             QStringLiteral("将素材库中的 %1 移至 Windows 回收站？").arg(file.name),
             QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    try {
        if (synthesis_->importing())
            fail(QStringLiteral("正在导入声音，请等待导入完成。"));
        const auto current = readLibrary(QSqlDatabase::database(), root_, false);
        if (referenced(current.files.value(key)))
            fail(QStringLiteral("此素材已有新引用。请查看刷新后的关联记录。"));
        const auto path = checkedPath(file.path);
        if (!isWithin(path, root_ + "/assets") || !QFileInfo(path).isFile() || !QFile::moveToTrash(path))
            fail(QStringLiteral("无法将素材移至 Windows 回收站。"));
        reload();
    } catch (const std::exception &error) {
        status_->setText(QString::fromUtf8(error.what()));
    }
}

}

QWidget *createLibraryPage(const QString &root, const QString &defaultOutput, TaskScheduler &scheduler,
                           SynthesisPanel *synthesis, QTabWidget *tabs)
{
    return new LibraryPage(root, defaultOutput, scheduler, synthesis, tabs);
}

}
