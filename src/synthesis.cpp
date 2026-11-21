#include "synthesis.h"
#include "engine.h"
#include "jobs.h"
#include "ui.h"

#include <QAudioOutput>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QTemporaryFile>
#include <QUuid>
#include <QtConcurrentRun>
#include <stdexcept>
#include <utility>

namespace ez {
namespace {

const QStringList languages{QStringLiteral("中文"), QStringLiteral("英文"), QStringLiteral("日文"),
    QStringLiteral("粤语"), QStringLiteral("韩文"), QStringLiteral("多语种混合")};

void execute(QSqlQuery &query)
{
    if (!query.exec())
        throw std::runtime_error(query.lastError().text().toStdString());
}

QString storeFile(const QString &path, const QString &directory)
{
    QFile source(checkedPath(path));
    if (!source.open(QIODevice::ReadOnly) || source.size() <= 0)
        throw std::runtime_error(QStringLiteral("无法读取文件：%1").arg(path).toStdString());
    const auto parent = checkedPath(directory);
    if (!QDir().mkpath(parent))
        throw std::runtime_error(QStringLiteral("无法创建素材目录").toStdString());

    QTemporaryFile temporary(parent + "/.import-XXXXXX");
    if (!temporary.open())
        throw std::runtime_error(temporary.errorString().toStdString());
    QCryptographicHash digest(QCryptographicHash::Sha256);
    for (;;) {
        const auto block = source.read(1024 * 1024);
        if (source.error() != QFileDevice::NoError)
            throw std::runtime_error(source.errorString().toStdString());
        if (block.isEmpty())
            break;
        digest.addData(block);
        if (temporary.write(block) != block.size())
            throw std::runtime_error(temporary.errorString().toStdString());
    }
    const auto target = checkedPath(parent + "/" + QString::fromLatin1(digest.result().toHex())
        + "." + QFileInfo(path).suffix().toLower());
    if (!QFileInfo::exists(target)) {
        if (!temporary.flush() || !temporary.rename(target))
            throw std::runtime_error(temporary.errorString().toStdString());
        temporary.setAutoRemove(false);
    }
    return target;
}

QLineEdit *fileRow(QFormLayout *form, const QString &title, const QString &filter,
                   const QString &initial, QWidget *parent)
{
    auto *row = new QHBoxLayout;
    auto *path = new QLineEdit(initial, parent);
    auto *select = new QPushButton(QStringLiteral("选择文件"), parent);
    row->addWidget(path, 1);
    row->addWidget(select);
    form->addRow(title, row);
    QObject::connect(select, &QPushButton::clicked, parent, [parent, title, filter, path] {
        const auto chosen = QFileDialog::getOpenFileName(parent, title, {}, filter);
        if (!chosen.isEmpty())
            path->setText(chosen);
    });
    return path;
}

}

SynthesisPanel::SynthesisPanel(QString root, TaskScheduler &scheduler, std::function<QString()> launcher,
                               QWidget *parent)
    : QWidget(parent), root_(std::move(root)), scheduler_(scheduler), launcher_(std::move(launcher))
{
    QSqlQuery query;
    query.prepare("CREATE TABLE IF NOT EXISTS voices ("
                  "id TEXT PRIMARY KEY, name TEXT NOT NULL, settings BLOB NOT NULL,"
                  "verified_engine TEXT NOT NULL DEFAULT '')");
    execute(query);

    auto *layout = new QVBoxLayout(this);
    ui::pageHeader(layout, this, QStringLiteral("语音合成"),
        QStringLiteral("选一个声音和台词语言，输入台词后点「生成语音」。首次使用可先导入权重和参考音频。"));
    auto *selectors = new QHBoxLayout;
    voices_ = new QComboBox(this);
    voices_->setPlaceholderText(QStringLiteral("先导入一个声音"));
    voices_->setAccessibleName(QStringLiteral("声音预设"));
    language_ = new QComboBox(this);
    language_->addItems(languages);
    language_->setAccessibleName(QStringLiteral("台词语言"));
    auto *import = new QPushButton(QStringLiteral("导入声音"), this);
    auto *voiceLabel = new QLabel(QStringLiteral("声音"), this);
    voiceLabel->setObjectName("fieldLabel");
    selectors->addWidget(voiceLabel);
    selectors->addWidget(voices_, 1);
    auto *languageLabel = new QLabel(QStringLiteral("台词语言"), this);
    languageLabel->setObjectName("fieldLabel");
    selectors->addWidget(languageLabel);
    selectors->addWidget(language_);
    selectors->addWidget(import);
    layout->addLayout(selectors);

    text_ = new QPlainTextEdit(this);
    text_->setObjectName("scriptEditor");
    text_->setAccessibleName(QStringLiteral("合成台词"));
    text_->setPlaceholderText(QStringLiteral("在这里输入台词。批量生成时，每行输入一段。"));
    layout->addWidget(text_, 1);
    auto *parameters = new QHBoxLayout;
    batch_ = new QCheckBox(QStringLiteral("每行生成一条音频"), this);
    parameters->addWidget(batch_);
    auto *advanced = new QPushButton(QStringLiteral("高级参数"), this);
    advanced->setCheckable(true);
    parameters->addStretch(1);
    parameters->addWidget(advanced);
    layout->addLayout(parameters);
    auto *advancedPanel = new QWidget(this);
    advancedPanel->setObjectName("card");
    auto *advancedLayout = new QHBoxLayout(advancedPanel);
    advancedLayout->setContentsMargins(11, 8, 11, 8);
    speed_ = new QDoubleSpinBox(this);
    speed_->setRange(0.60, 1.65);
    speed_->setSingleStep(0.05);
    speed_->setValue(1);
    speed_->setPrefix(QStringLiteral("语速 "));
    seed_ = new QLineEdit(this);
    seed_->setPlaceholderText(QStringLiteral("随机种子（选填）"));
    seed_->setMaxLength(10);
    advancedLayout->addWidget(speed_);
    advancedLayout->addWidget(seed_, 1);
    layout->addWidget(advancedPanel);
    advancedPanel->hide();
    connect(advanced, &QPushButton::toggled, advancedPanel, &QWidget::setVisible);
    connect(advanced, &QPushButton::toggled, this, [advanced](bool open) {
        advanced->setText(open ? QStringLiteral("收起高级参数") : QStringLiteral("高级参数"));
    });

    auto *actions = new QHBoxLayout;
    auto *generate = new QPushButton(QStringLiteral("生成语音"), this);
    generate->setObjectName("primary");
    play_ = new QPushButton(QStringLiteral("播放"), this);
    export_ = new QPushButton(QStringLiteral("另存 WAV"), this);
    folder_ = new QPushButton(QStringLiteral("所在目录"), this);
    play_->setEnabled(false);
    export_->setEnabled(false);
    folder_->setEnabled(false);
    actions->addWidget(generate);
    actions->addStretch(1);
    actions->addWidget(play_);
    actions->addWidget(export_);
    actions->addWidget(folder_);
    layout->addLayout(actions);

    status_ = ui::hint({}, this);
    layout->addWidget(status_);
    player_ = new QMediaPlayer(this);
    player_->setAudioOutput(new QAudioOutput(player_));
    connect(import, &QPushButton::clicked, this, [this] { importVoice(); });
    connect(generate, &QPushButton::clicked, this, &SynthesisPanel::generate);
    connect(&scheduler_, &TaskScheduler::completed, this, &SynthesisPanel::receiveAudio);
    connect(&scheduler_, &TaskScheduler::failed, status_, &QLabel::setText);
    connect(play_, &QPushButton::clicked, this, [this] {
        if (player_->playbackState() == QMediaPlayer::PlayingState)
            player_->pause();
        else
            player_->play();
    });
    connect(folder_, &QPushButton::clicked, this, [this] {
        try {
            const auto parent = QFileInfo(checkedPath(audio_)).absolutePath();
            if (!QFileInfo(parent).isDir() || !QDesktopServices::openUrl(QUrl::fromLocalFile(parent)))
                status_->setText(QStringLiteral("无法打开音频所在目录"));
        } catch (const std::exception &error) {
            status_->setText(QString::fromUtf8(error.what()));
        }
    });
    connect(player_, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        play_->setText(state == QMediaPlayer::PlayingState ? QStringLiteral("暂停") : QStringLiteral("播放"));
    });
    connect(export_, &QPushButton::clicked, this, [this] {
        auto target = QFileDialog::getSaveFileName(this, QStringLiteral("另存 WAV"),
                                                  QFileInfo(audio_).fileName(), "WAV (*.wav)");
        if (target.isEmpty())
            return;
        try {
            if (!target.endsWith(".wav", Qt::CaseInsensitive))
                target += ".wav";
            QFile source(checkedPath(audio_));
            QSaveFile output(checkedPath(target));
            if (!source.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
                throw std::runtime_error(QStringLiteral("无法读取音频或写入目标文件").toStdString());
            while (!source.atEnd()) {
                const auto chunk = source.read(1024 * 1024);
                if (source.error() != QFileDevice::NoError || output.write(chunk) != chunk.size())
                    throw std::runtime_error(QStringLiteral("写入中断，请检查磁盘空间").toStdString());
            }
            if (!output.commit())
                throw std::runtime_error(output.errorString().toStdString());
        } catch (const std::exception &error) {
            status_->setText(QString::fromUtf8(error.what()));
        }
    });
    connect(&importer_, &QFutureWatcher<QJsonObject>::finished, this, [this] {
        importing_ = false;
        try {
            auto result = importer_.result();
            if (result.contains("error"))
                throw std::runtime_error(result.value("error").toString().toStdString());
            const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            QSqlQuery query;
            query.prepare("INSERT INTO voices(id,name,settings) VALUES(?,?,?)");
            query.addBindValue(id);
            query.addBindValue(result.value("name").toString());
            query.addBindValue(QJsonDocument(result).toJson(QJsonDocument::Compact));
            execute(query);
            refreshVoices(id);
            status_->setText(QStringLiteral("声音已保存。输入短句生成试听，完成兼容性验证。"));
        } catch (const std::exception &error) {
            status_->setText(QString::fromUtf8(error.what()));
        }
    });
    refreshVoices();
}

SynthesisPanel::~SynthesisPanel()
{
    importer_.waitForFinished();
}

void SynthesisPanel::playAudio(const QString &path)
{
    audio_ = checkedPath(path);
    if (!QFileInfo(audio_).isFile())
        throw std::runtime_error(QStringLiteral("历史音频文件已移动或删除").toStdString());
    player_->setSource(QUrl::fromLocalFile(audio_));
    play_->setEnabled(true);
    export_->setEnabled(true);
    folder_->setEnabled(true);
    player_->play();
}

void SynthesisPanel::loadRequest(const QJsonObject &request)
{
    const auto index = voices_->findData(request.value("voice_id").toString());
    if (index >= 0)
        voices_->setCurrentIndex(index);
    text_->setPlainText(request.value("text").toString());
    language_->setCurrentText(request.value("language").toString());
    speed_->setValue(request.value("speed").toDouble(1));
    const auto seed = request.value("seed").toInteger(-1);
    seed_->setText(seed < 0 ? QString() : QString::number(seed));
    batch_->setChecked(false);
}

void SynthesisPanel::refreshVoices(const QString &selected)
{
    const auto current = selected.isEmpty() ? voices_->currentData().toString() : selected;
    QSqlQuery query;
    query.prepare("SELECT id,name,verified_engine FROM voices ORDER BY name");
    execute(query);
    voices_->clear();
    while (query.next()) {
        const auto verified = query.value(2).toString();
        voices_->addItem(query.value(1).toString() + (verified.isEmpty()
            ? QStringLiteral(" · 待验证") : QStringLiteral(" · 已验证")), query.value(0));
        voices_->setItemData(voices_->count() - 1, verified, Qt::ToolTipRole);
    }
    const int index = voices_->findData(current);
    voices_->setCurrentIndex(index >= 0 ? index : (voices_->count() ? 0 : -1));
}

void SynthesisPanel::importVoice(QJsonObject defaults)
{
    if (importing_) {
        status_->setText(QStringLiteral("正在导入声音，请稍候"));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("导入声音"));
    dialog.resize(650, 430);
    auto *form = new QFormLayout(&dialog);
    form->addRow(ui::hint(QStringLiteral("选择同一个声音的两份配套权重，再添加一段参考录音和对应原文。"), &dialog));
    auto *name = new QLineEdit(defaults.value("name").toString(), &dialog);
    auto *family = new QComboBox(&dialog);
    family->addItems({"v5turbo", "v5dev", "v2Pro", "v2ProPlus", "v2"});
    if (defaults.contains("family"))
        family->setCurrentText(defaults.value("family").toString());
    form->addRow(QStringLiteral("声音名称"), name);
    form->addRow(QStringLiteral("模型家族"), family);
    auto *gpt = fileRow(form, QStringLiteral("GPT 权重"), "GPT (*.ckpt)", defaults.value("gpt").toString(), &dialog);
    auto *sovits = fileRow(form, QStringLiteral("SoVITS 权重"), "SoVITS (*.pth)", defaults.value("sovits").toString(), &dialog);
    auto *ref = fileRow(form, QStringLiteral("参考音频"), "Audio (*.wav *.flac *.mp3)",
                        defaults.value("reference").toString(), &dialog);
    auto *preview = new QPushButton(QStringLiteral("试听参考音频"), &dialog);
    form->addRow(preview);
    auto *referencePlayer = new QMediaPlayer(&dialog);
    referencePlayer->setAudioOutput(new QAudioOutput(referencePlayer));
    connect(preview, &QPushButton::clicked, &dialog, [referencePlayer, ref, &dialog] {
        if (referencePlayer->playbackState() == QMediaPlayer::PlayingState) {
            referencePlayer->stop();
            return;
        }
        try {
            const auto source = checkedPath(ref->text());
            if (!QFileInfo(source).isFile())
                throw std::runtime_error(QStringLiteral("请选择有效的参考音频").toStdString());
            referencePlayer->setSource(QUrl::fromLocalFile(source));
            referencePlayer->play();
        } catch (const std::exception &error) {
            QMessageBox::warning(&dialog, QStringLiteral("无法试听"), QString::fromUtf8(error.what()));
        }
    });
    auto *prompt = new QPlainTextEdit(defaults.value("prompt").toString(), &dialog);
    prompt->setPlaceholderText(QStringLiteral("填写参考音频中实际说出的内容，推荐 3 至 10 秒的清晰录音"));
    form->addRow(QStringLiteral("参考文本"), prompt);
    auto *lang = new QComboBox(&dialog);
    lang->addItems(languages);
    if (defaults.contains("prompt_language"))
        lang->setCurrentText(defaults.value("prompt_language").toString());
    form->addRow(QStringLiteral("参考语言"), lang);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("保存声音"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;

    QJsonObject values{{"name", name->text().trimmed()}, {"family", family->currentText()},
        {"gpt", gpt->text()}, {"sovits", sovits->text()}, {"reference", ref->text()},
        {"prompt", prompt->toPlainText().trimmed()}, {"prompt_language", lang->currentText()}};
    if (defaults.contains("id") && values.value("family") == defaults.value("family")
        && values.value("gpt") == defaults.value("gpt")
        && values.value("sovits") == defaults.value("sovits")) {
        values["project_id"] = defaults.value("id");
    }
    if (values.value("name").toString().isEmpty() || values.value("prompt").toString().isEmpty()
        || !gpt->text().endsWith(".ckpt", Qt::CaseInsensitive) || !sovits->text().endsWith(".pth", Qt::CaseInsensitive)
        || ref->text().isEmpty()) {
        status_->setText(QStringLiteral("请填写名称及参考文本，并选择两份对应权重和参考音频"));
        return;
    }
    status_->setText(QStringLiteral("正在复制和校验声音文件…"));
    importing_ = true;
    const auto root = root_;
    importer_.setFuture(QtConcurrent::run([root, values]() mutable {
        try {
            qint64 bytes = 0;
            for (const auto &key : {"gpt", "sovits", "reference"})
                bytes += QFileInfo(checkedPath(values.value(key).toString())).size();
            if (QStorageInfo(root).bytesAvailable() < bytes)
                throw std::runtime_error(QStringLiteral("素材库磁盘空间不足").toStdString());
            for (const auto &key : {"gpt", "sovits", "reference"})
                values[QString::fromLatin1(key) + "_name"] = QFileInfo(values.value(key).toString()).fileName();
            const auto models = root + "/assets/models/" + values.value("family").toString();
            values["gpt"] = storeFile(values.value("gpt").toString(), models + "/GPT");
            values["sovits"] = storeFile(values.value("sovits").toString(), models + "/SoVITS");
            values["reference"] = storeFile(values.value("reference").toString(), root + "/assets/audio");
            return values;
        } catch (const std::exception &error) {
            return QJsonObject{{"error", QString::fromUtf8(error.what())}};
        }
    }));
}

void SynthesisPanel::generate()
{
    try {
        const auto launcher = launcher_();
        const auto voice = voices_->currentData().toString();
        if (launcher.isEmpty()) {
            status_->setText(QStringLiteral("请在「设置」中添加并选择引擎"));
            return;
        }
        if (voice.isEmpty() || text_->toPlainText().trimmed().isEmpty()) {
            status_->setText(QStringLiteral("请先选择声音，再输入台词"));
            return;
        }
        const auto seedText = seed_->text().trimmed();
        bool valid = false;
        const auto seed = seedText.toLongLong(&valid);
        if (!seedText.isEmpty() && (!valid || seed < 0 || seed > 4294967295LL)) {
            status_->setText(QStringLiteral("随机种子应为 0 至 4294967295"));
            return;
        }
        QSqlQuery query;
        query.prepare("SELECT settings FROM voices WHERE id=?");
        query.addBindValue(voice);
        execute(query);
        if (!query.next())
            throw std::runtime_error(QStringLiteral("声音预设已不存在").toStdString());
        auto request = QJsonDocument::fromJson(query.value(0).toByteArray()).object();
        request["operation"] = "synthesize";
        request["voice_id"] = voice;
        request["language"] = language_->currentText();
        request["speed"] = speed_->value();
        request["seed"] = seedText.isEmpty() ? -1LL : seed;
        const auto text = text_->toPlainText();
        const auto lines = batch_->isChecked() ? text.split('\n', Qt::SkipEmptyParts) : QStringList{text};
        int count = 0;
        for (const auto &line : lines) {
            if (line.trimmed().isEmpty())
                continue;
            request["text"] = line.trimmed();
            scheduler_.enqueue(launcher, request);
            ++count;
        }
        status_->setText(QStringLiteral("已排队 %1 条台词，可在任务记录查看进度。").arg(count));
    } catch (const std::exception &error) {
        status_->setText(QString::fromUtf8(error.what()));
    }
}

void SynthesisPanel::receiveAudio(const QString &id, const QJsonObject &result)
{
    if (!result.contains("audio"))
        return;
    audio_ = result.value("audio").toString();
    player_->setSource(QUrl::fromLocalFile(audio_));
    play_->setEnabled(true);
    export_->setEnabled(true);
    folder_->setEnabled(true);
    status_->setText(QStringLiteral("语音已生成 · %1 秒").arg(result.value("duration").toDouble(), 0, 'f', 2));
    QSqlQuery query;
    query.prepare("SELECT launcher,request FROM jobs WHERE id=?");
    query.addBindValue(id);
    if (!query.exec() || !query.next())
        return;
    const auto launcher = query.value(0).toString();
    const auto voice = QJsonDocument::fromJson(query.value(1).toByteArray()).object().value("voice_id").toString();
    query.finish();
    query.prepare("UPDATE voices SET verified_engine=? WHERE id=?");
    query.addBindValue(launcher);
    query.addBindValue(voice);
    if (query.exec())
        refreshVoices();
}

}
