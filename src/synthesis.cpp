#include "synthesis.h"
#include "engine.h"
#include "jobs.h"
#include "ui.h"
#include "voiceverification.h"

#include <QAudioOutput>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPromise>
#include <QPushButton>
#include <QSaveFile>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSpinBox>
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
const QStringList splits{QStringLiteral("不切"), QStringLiteral("凑四句一切"), QStringLiteral("凑50字一切"),
    QStringLiteral("按中文句号。切"), QStringLiteral("按英文句号.切"), QStringLiteral("按标点符号切")};
constexpr auto defaultSplit = 1;
constexpr auto defaultTopK = 15;
constexpr auto defaultTopP = 1.0;
constexpr auto defaultTemperature = 1.0;
constexpr auto voiceSettingsRole = Qt::UserRole + 1;

QString supportedLanguage(const QString &language)
{
    return languages.contains(language) ? language : languages.constFirst();
}

QStringList synthesisLines(const QString &text)
{
    QStringList lines;
    for (const auto &line : text.split('\n', Qt::SkipEmptyParts)) {
        const auto trimmed = line.trimmed();
        if (!trimmed.isEmpty())
            lines.append(trimmed);
    }
    return lines;
}

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
    connect(voices_, &QComboBox::currentIndexChanged, this, &SynthesisPanel::applyVoiceDefaults);
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
    auto *lineCount = ui::hint({}, this);
    lineCount->setObjectName("batchLineCount");
    lineCount->setWordWrap(false);
    parameters->addWidget(lineCount);
    advanced_ = new QPushButton(QStringLiteral("高级参数"), this);
    advanced_->setCheckable(true);
    parameters->addStretch(1);
    parameters->addWidget(advanced_);
    layout->addLayout(parameters);
    auto *advancedPanel = new QWidget(this);
    advancedPanel->setObjectName("card");
    auto *advancedLayout = new QGridLayout(advancedPanel);
    advancedLayout->setContentsMargins(11, 8, 11, 8);
    advancedLayout->setHorizontalSpacing(12);
    advancedLayout->setVerticalSpacing(8);
    const auto field = [advancedLayout, advancedPanel](int row, int column, const QString &name, QWidget *control) {
        auto *label = new QLabel(name, advancedPanel);
        label->setObjectName("fieldLabel");
        label->setBuddy(control);
        control->setAccessibleName(name);
        advancedLayout->addWidget(label, row, 2 * column);
        advancedLayout->addWidget(control, row, 2 * column + 1);
        advancedLayout->setColumnStretch(2 * column + 1, 1);
    };
    split_ = new QComboBox(advancedPanel);
    split_->addItems(splits);
    split_->setCurrentIndex(defaultSplit);
    field(0, 0, QStringLiteral("切句"), split_);
    speed_ = new QDoubleSpinBox(advancedPanel);
    speed_->setRange(0.60, 1.65);
    speed_->setSingleStep(0.05);
    speed_->setValue(1);
    field(0, 1, QStringLiteral("语速"), speed_);
    seed_ = new QLineEdit(advancedPanel);
    seed_->setPlaceholderText(QStringLiteral("留空为随机"));
    seed_->setMaxLength(10);
    field(0, 2, QStringLiteral("种子"), seed_);
    topK_ = new QSpinBox(advancedPanel);
    topK_->setRange(1, 100);
    topK_->setValue(defaultTopK);
    topK_->setToolTip(QStringLiteral("每步采样最多考虑的候选数。"));
    field(1, 0, QStringLiteral("Top K"), topK_);
    topP_ = new QDoubleSpinBox(advancedPanel);
    topP_->setRange(0.0, 1.0);
    topP_->setSingleStep(0.05);
    topP_->setValue(defaultTopP);
    topP_->setToolTip(QStringLiteral("控制每步采样考虑的候选累计概率。"));
    field(1, 1, QStringLiteral("Top P"), topP_);
    temperature_ = new QDoubleSpinBox(advancedPanel);
    temperature_->setRange(0.05, 1.0);
    temperature_->setSingleStep(0.05);
    temperature_->setValue(defaultTemperature);
    temperature_->setToolTip(QStringLiteral("较低温度倾向于选择概率较高的候选。"));
    field(1, 2, QStringLiteral("温度"), temperature_);
    saveDefaults_ = new QPushButton(QStringLiteral("保存为此声音的默认参数"), advancedPanel);
    saveDefaults_->setEnabled(false);
    saveDefaults_->setToolTip(QStringLiteral("保存当前台词语言、切句、语速、种子与采样参数。"));
    advancedLayout->addWidget(saveDefaults_, 2, 0, 1, 6, Qt::AlignRight);
    connect(saveDefaults_, &QPushButton::clicked, this, &SynthesisPanel::saveVoiceDefaults);
    layout->addWidget(advancedPanel);
    advancedPanel->hide();
    connect(advanced_, &QPushButton::toggled, advancedPanel, &QWidget::setVisible);
    connect(advanced_, &QPushButton::toggled, this, [this](bool open) {
        advanced_->setText(open ? QStringLiteral("收起高级参数") : QStringLiteral("高级参数"));
    });

    auto *actions = new QHBoxLayout;
    auto *generate = new QPushButton(QStringLiteral("生成语音"), this);
    generate->setObjectName("primary");
    const auto updateMode = [this, lineCount, generate] {
        const bool batch = batch_->isChecked();
        const auto count = batch ? synthesisLines(text_->toPlainText()).size() : 0;
        lineCount->setVisible(batch);
        lineCount->setText(QStringLiteral("%1 条台词，空行会跳过").arg(count));
        generate->setText(batch ? QStringLiteral("提交批量任务") : QStringLiteral("生成语音"));
        generate->setEnabled(batch ? count > 0 : !text_->toPlainText().trimmed().isEmpty());
    };
    updateMode();
    connect(batch_, &QCheckBox::toggled, this, updateMode);
    connect(text_, &QPlainTextEdit::textChanged, this, updateMode);
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

    auto *batchControls = new QHBoxLayout;
    batchSummary_ = ui::hint({}, this);
    batchSummary_->setObjectName("stageHint");
    viewBatch_ = new QPushButton(QStringLiteral("查看本批任务"), this);
    batchControls->addWidget(batchSummary_, 1);
    batchControls->addWidget(viewBatch_);
    layout->addLayout(batchControls);
    batchSummary_->hide();
    viewBatch_->hide();
    connect(viewBatch_, &QPushButton::clicked, this, [this] { emit viewBatchRequested(submittedBatch_); });

    status_ = ui::hint({}, this);
    status_->setObjectName("synthesisStatus");
    layout->addWidget(status_);
    player_ = new QMediaPlayer(this);
    player_->setAudioOutput(new QAudioOutput(player_));
    connect(import, &QPushButton::clicked, this, [this] { importVoice(); });
    connect(generate, &QPushButton::clicked, this, &SynthesisPanel::generate);
    connect(&scheduler_, &TaskScheduler::completed, this, &SynthesisPanel::receiveAudio);
    connect(&scheduler_, &TaskScheduler::changed, this, &SynthesisPanel::refreshSubmission);
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
        if (exporter_.isRunning())
            return;
        const auto audio = audio_;
        auto target = QFileDialog::getSaveFileName(this, QStringLiteral("另存 WAV"),
                                                  QFileInfo(audio).fileName(), "WAV (*.wav)");
        if (target.isEmpty())
            return;
        if (!target.endsWith(".wav", Qt::CaseInsensitive))
            target += ".wav";
        export_->setText(QStringLiteral("正在另存…"));
        export_->setEnabled(false);
        exporter_.setFuture(QtConcurrent::run([audio, target](QPromise<QString> &promise) {
            try {
                QFile source(checkedPath(audio));
                QSaveFile output(checkedPath(target));
                if (source.fileName().compare(output.fileName(), Qt::CaseInsensitive) == 0)
                    throw std::runtime_error(QStringLiteral("请选择其他路径，当前 WAV 已在该位置").toStdString());
                if (!source.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly))
                    throw std::runtime_error(QStringLiteral("无法读取音频或写入目标文件").toStdString());
                while (!promise.isCanceled() && !source.atEnd()) {
                    const auto chunk = source.read(1024 * 1024);
                    if (source.error() != QFileDevice::NoError || output.write(chunk) != chunk.size())
                        throw std::runtime_error(QStringLiteral("写入中断，请检查磁盘空间").toStdString());
                }
                source.close();
                if (!promise.isCanceled()) {
                    if (!output.commit())
                        throw std::runtime_error(output.errorString().toStdString());
                    promise.addResult(QStringLiteral("WAV 已保存到 %1").arg(QDir::toNativeSeparators(target)));
                }
            } catch (const std::exception &error) {
                promise.addResult(QString::fromUtf8(error.what()));
            }
        }));
    });
    connect(&exporter_, &QFutureWatcher<QString>::finished, this, [this] {
        export_->setText(QStringLiteral("另存 WAV"));
        export_->setEnabled(!audio_.isEmpty());
        if (!exporter_.isCanceled())
            status_->setText(exporter_.result());
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
    exporter_.cancel();
    exporter_.waitForFinished();
}

void SynthesisPanel::playAudio(const QString &path)
{
    audio_ = checkedPath(path);
    if (!QFileInfo(audio_).isFile())
        throw std::runtime_error(QStringLiteral("历史音频文件已移动或删除").toStdString());
    player_->setSource(QUrl::fromLocalFile(audio_));
    play_->setEnabled(true);
    play_->setToolTip({});
    export_->setEnabled(!exporter_.isRunning());
    folder_->setEnabled(true);
    player_->play();
}

void SynthesisPanel::loadRequest(const QJsonObject &request)
{
    const auto index = voices_->findData(request.value("voice_id").toString());
    if (index >= 0)
        voices_->setCurrentIndex(index);
    text_->setPlainText(request.value("text").toString());
    language_->setCurrentText(supportedLanguage(request.value("language").toString()));
    applySynthesisOptions(request);
    if (split_->currentIndex() != defaultSplit || speed_->value() != 1 || !seed_->text().isEmpty()
        || topK_->value() != defaultTopK || topP_->value() != defaultTopP || temperature_->value() != defaultTemperature)
        advanced_->setChecked(true);
    batch_->setChecked(false);
}

void SynthesisPanel::applySynthesisOptions(const QJsonObject &options)
{
    speed_->setValue(options.value("speed").toDouble(1));
    const auto split = split_->findText(options.value("split").toString());
    split_->setCurrentIndex(split < 0 ? defaultSplit : split);
    topK_->setValue(options.value("top_k").toInt(defaultTopK));
    topP_->setValue(options.value("top_p").toDouble(defaultTopP));
    temperature_->setValue(options.value("temperature").toDouble(defaultTemperature));
    const auto seed = options.value("seed").toInteger(-1);
    seed_->setText(seed < 0 ? QString() : QString::number(seed));
}

void SynthesisPanel::applyVoiceDefaults()
{
    const auto settings = voices_->currentData(voiceSettingsRole).toJsonObject();
    language_->setCurrentText(supportedLanguage(settings.value("default_language").toString()));
    applySynthesisOptions(settings);
    saveDefaults_->setEnabled(!voices_->currentData().toString().isEmpty());
}

void SynthesisPanel::refreshVoices(const QString &selected)
{
    const auto previous = voices_->currentData().toString();
    const auto current = selected.isEmpty() ? previous : selected;
    auto query = voicesWithVerification();
    if (query.lastError().isValid())
        throw std::runtime_error(query.lastError().text().toStdString());
    const auto launcher = launcher_();
    const QSignalBlocker block(voices_);
    voices_->clear();
    while (query.next()) {
        const auto settings = QJsonDocument::fromJson(query.value(2).toByteArray()).object();
        const auto legacy = query.value(3).toString();
        const auto record = QJsonDocument::fromJson(query.value(4).toByteArray()).object();
        const auto state = voiceVerificationStatus(settings, record, legacy, launcher);
        voices_->addItem(query.value(1).toString() + QStringLiteral(" · ") + state, query.value(0));
        const auto date = QDateTime::fromString(record.value("verified_at").toString(), Qt::ISODateWithMs).toLocalTime();
        auto details = state;
        if (date.isValid()) {
            details += QStringLiteral("\n上次试音 · %1\n%2")
                .arg(date.toString("yyyy-MM-dd HH:mm:ss"), record.value("launcher").toString());
        }
        voices_->setItemData(voices_->count() - 1, details, Qt::ToolTipRole);
        voices_->setItemData(voices_->count() - 1, settings, voiceSettingsRole);
    }
    const int index = voices_->findData(current);
    voices_->setCurrentIndex(index >= 0 ? index : (voices_->count() ? 0 : -1));
    if (previous != voices_->currentData().toString())
        applyVoiceDefaults();
}

void SynthesisPanel::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    try {
        refreshVoices();
    } catch (const std::exception &error) {
        status_->setText(QString::fromUtf8(error.what()));
    }
}

QJsonObject SynthesisPanel::selectedVoiceSettings() const
{
    QSqlQuery query;
    query.prepare("SELECT settings FROM voices WHERE id=?");
    query.addBindValue(voices_->currentData().toString());
    execute(query);
    if (!query.next())
        throw std::runtime_error(QStringLiteral("声音预设已不存在").toStdString());
    return QJsonDocument::fromJson(query.value(0).toByteArray()).object();
}

QJsonObject SynthesisPanel::synthesisOptions() const
{
    const auto seedText = seed_->text().trimmed();
    bool valid = false;
    const auto seed = seedText.toLongLong(&valid);
    if (!seedText.isEmpty() && (!valid || seed < 0 || seed > 4294967295LL))
        throw std::runtime_error(QStringLiteral("随机种子应为 0 至 4294967295").toStdString());
    return {{"speed", speed_->value()}, {"split", split_->currentText()}, {"top_k", topK_->value()},
        {"top_p", topP_->value()}, {"temperature", temperature_->value()}, {"seed", seedText.isEmpty() ? -1LL : seed}};
}

void SynthesisPanel::saveVoiceDefaults()
{
    try {
        auto settings = selectedVoiceSettings();
        const auto options = synthesisOptions();
        for (auto option = options.constBegin(); option != options.constEnd(); ++option)
            settings.insert(option.key(), option.value());
        settings["default_language"] = language_->currentText();
        QSqlQuery query;
        query.prepare("UPDATE voices SET settings=? WHERE id=?");
        query.addBindValue(QJsonDocument(settings).toJson(QJsonDocument::Compact));
        query.addBindValue(voices_->currentData().toString());
        execute(query);
        refreshVoices();
        status_->setText(QStringLiteral("已保存当前声音的默认参数。"));
    } catch (const std::exception &error) {
        status_->setText(QString::fromUtf8(error.what()));
    }
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
    auto *target = new QComboBox(&dialog);
    target->addItems(languages);
    target->setCurrentText(supportedLanguage(defaults.value("default_language")
        .toString(defaults.value("prompt_language").toString())));
    form->addRow(QStringLiteral("默认台词语言"), target);
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
        {"prompt", prompt->toPlainText().trimmed()}, {"prompt_language", lang->currentText()},
        {"default_language", target->currentText()}};
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
        refreshVoices();
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
        auto request = selectedVoiceSettings();
        const auto options = synthesisOptions();
        for (auto option = options.constBegin(); option != options.constEnd(); ++option)
            request.insert(option.key(), option.value());
        request["operation"] = "synthesize";
        request["voice_id"] = voice;
        request["language"] = language_->currentText();
        if (batch_->isChecked()) {
            QList<QJsonObject> requests;
            for (const auto &line : synthesisLines(text_->toPlainText())) {
                request["text"] = line;
                requests.append(request);
            }
            submittedBatch_ = scheduler_.enqueueSynthesisBatch(launcher, requests);
            submittedJob_.clear();
            status_->clear();
        } else {
            request["text"] = text_->toPlainText().trimmed();
            submittedJob_ = scheduler_.enqueue(launcher, request);
            submittedBatch_.clear();
        }
        audio_.clear();
        player_->setSource({});
        play_->setEnabled(false);
        play_->setToolTip({});
        export_->setEnabled(false);
        folder_->setEnabled(false);
        status_->setToolTip({});
        refreshSubmission();
    } catch (const std::exception &error) {
        status_->setText(QString::fromUtf8(error.what()));
    }
}

void SynthesisPanel::refreshSubmission()
{
    const bool batch = !submittedBatch_.isEmpty();
    batchSummary_->setVisible(batch);
    viewBatch_->setVisible(batch);
    if (!batch && submittedJob_.isEmpty())
        return;
    QSqlQuery query;
    if (!batch) {
        query.prepare("SELECT state,message FROM jobs WHERE id=?");
        query.addBindValue(submittedJob_);
        if (!query.exec() || !query.next())
            return;
        const auto state = query.value(0).toString();
        if (state == "queued")
            status_->setText(QStringLiteral("已加入队列，等待引擎或前面的任务。"));
        else if (state == "running")
            status_->setText(query.value(1).toString().isEmpty() ? QStringLiteral("正在生成语音…") : query.value(1).toString());
        else if (state == "failed")
            status_->setText(query.value(1).toString());
        else if (state == "cancelled")
            status_->setText(QStringLiteral("任务已取消。"));
        return;
    }

    query.prepare("SELECT state,message FROM jobs WHERE json_extract(CAST(request AS TEXT),'$.batch_id')=? ORDER BY rowid");
    query.addBindValue(submittedBatch_);
    if (!query.exec())
        return;
    QMap<QString, int> counts;
    QString failure;
    int total = 0;
    while (query.next()) {
        const auto state = query.value(0).toString();
        ++counts[state];
        ++total;
        if (state == "failed" && failure.isEmpty())
            failure = query.value(1).toString();
    }
    const int failed = counts.value("failed");
    const int interrupted = counts.value("interrupted");
    const auto summary = QStringLiteral("本批 %1 条 · 成功 %2 · 失败 %3 · 排队 %4 · 运行 %5 · 已取消 %6")
        .arg(total).arg(counts.value("succeeded")).arg(failed)
        .arg(counts.value("queued")).arg(counts.value("running")).arg(counts.value("cancelled"));
    batchSummary_->setText(interrupted > 0 ? summary + QStringLiteral(" · 中断 %1").arg(interrupted) : summary);
    if (!failure.isEmpty()) {
        status_->setText(failure.section('\n', 0, 1));
        status_->setToolTip(failure);
    }
}

void SynthesisPanel::receiveAudio(const QString &id, const QJsonObject &result)
{
    if (!result.contains("audio"))
        return;
    QSqlQuery query;
    query.prepare("SELECT request FROM jobs WHERE id=?");
    query.addBindValue(id);
    if (!query.exec() || !query.next())
        return;
    const auto request = QJsonDocument::fromJson(query.value(0).toByteArray()).object();
    const bool batch = !submittedBatch_.isEmpty();
    const bool visibleResult = batch ? request.value("batch_id").toString() == submittedBatch_
        : submittedJob_.isEmpty() || id == submittedJob_;
    if (visibleResult) {
        audio_ = result.value("audio").toString();
        player_->setSource(QUrl::fromLocalFile(audio_));
        play_->setEnabled(true);
        play_->setToolTip(batch ? QStringLiteral("播放本批最近生成的成品 · 第 %1 条")
            .arg(request.value("batch_index").toInt()) : QString());
        export_->setEnabled(!exporter_.isRunning());
        folder_->setEnabled(true);
        if (!batch)
            status_->setText(QStringLiteral("语音已生成 · %1 秒").arg(result.value("duration").toDouble(), 0, 'f', 2));
    }
    if (result.contains("voice_verification")) {
        query.finish();
        try {
            refreshVoices();
        } catch (const std::exception &error) {
            status_->setText(QString::fromUtf8(error.what()));
        }
    }
}

}
