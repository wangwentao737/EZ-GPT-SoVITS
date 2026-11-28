#include "recordings.h"
#include "scriptlines.h"
#include "waveexport.h"
#include "engine.h"
#include "compatdraft.h"
#include "jobs.h"
#include "settings.h"
#include "synthesis.h"
#include "training.h"
#include "ui.h"
#include "workspacepaths.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QAudioDevice>
#include <QAudioOutput>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMediaDevices>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QNetworkProxy>
#include <QPushButton>
#include <QSaveFile>
#include <QScreen>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>
#include <QTimer>
#include <QtEndian>
#include <stdexcept>
#include <thread>

using namespace ez;

namespace {

void check(bool condition, const QString &detail)
{
    if (!condition)
        throw std::runtime_error(detail.toStdString());
}

QString writeWave(const QString &path, int rate, int milliseconds, qint16 sample)
{
    check(QDir().mkpath(QFileInfo(path).path()), "Cannot create fixture directory");
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "Cannot create WAV fixture");
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    const quint32 frames = rate * milliseconds / 1000;
    const quint32 bytes = frames * 2;
    stream.writeRawData("RIFF", 4);
    stream << quint32(bytes + 36);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(1) << quint32(rate) << quint32(rate * 2) << quint16(2) << quint16(16);
    stream.writeRawData("data", 4);
    stream << bytes;
    for (quint32 frame = 0; frame < frames; ++frame)
        stream << sample;
    check(stream.status() == QDataStream::Ok, "Cannot write WAV fixture");
    return path;
}

QString sha(const QString &path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    check(file.open(QIODevice::ReadOnly) && hash.addData(&file), "Cannot hash fixture");
    return QString::fromLatin1(hash.result().toHex());
}

void comboPopupPlacement()
{
    QApplication::setStyle("Fusion");
    QWidget window;
    window.resize(240, 100);
    QComboBox choice(&window);
    choice.setGeometry(12, 12, 135, 34);
    choice.addItems({QStringLiteral("深色"), QStringLiteral("浅色")});
    window.move(window.screen()->availableGeometry().topLeft() + QPoint(80, 80));
    window.show();
    check(QTest::qWaitForWindowExposed(&window), "Cannot show the popup test window");

    for (const auto theme : {ui::Theme::Dark, ui::Theme::Light}) {
        ui::applyTheme(theme);
        for (int index = 0; index < choice.count(); ++index) {
            choice.setCurrentIndex(index);
            choice.showPopup();
            const auto *popup = choice.view()->window();
            const auto anchor = choice.mapToGlobal(QPoint(0, choice.height()));
            check(popup->isVisible() && (popup->geometry().topLeft() - anchor).manhattanLength() <= 2,
                "The popup moved away from the bottom edge when its selection or theme changed");
            QTest::keyClick(choice.view(), Qt::Key_Escape);
            check(!popup->isVisible() && choice.currentIndex() == index, "Escape must close the list without changing the selection");
        }
    }

    const auto screen = window.screen()->availableGeometry();
    window.move(screen.left() + 80, screen.bottom() - 48);
    choice.showPopup();
    const auto *popup = choice.view()->window();
    check(popup->isVisible() && popup->geometry().bottom() <= choice.mapToGlobal(QPoint()).y()
        && screen.contains(popup->geometry()), "The popup must open above the field if the screen has no room below it");
    choice.hidePopup();
}

void writeFixture(const QString &path, const QByteArray &contents = "fixture")
{
    check(QDir().mkpath(QFileInfo(path).path()), "Cannot create compatibility fixture directory");
    QFile file(path);
    check(file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size(), "Cannot write compatibility fixture");
}

void engineCompatibility(const QDir &root)
{
    const auto data = root.filePath("compatibility");
    const auto package = data + "/first/GPT-SoVITS";
    const auto sameName = data + "/second/GPT-SoVITS";
    const QString launcher = "/go-webui.bat";
    const QString inference = "/GPT_SoVITS/inference_webui_fast.py";
    const QString speaker = "/GPT_SoVITS/pretrained_models/sv/pretrained_eres2netv2w24s4ep4.ckpt";
    for (const auto &file : QStringList{launcher, "/webui.py", "/config.py", "/runtime/python.exe", inference, speaker,
             "/tools/slice_audio.py", "/GPT_SoVITS/pretrained_models/chinese-hubert-base/config.json",
             "/GPT_SoVITS/pretrained_models/chinese-hubert-base/pytorch_model.bin",
             "/GPT_SoVITS/pretrained_models/chinese-roberta-wwm-ext-large/config.json",
             "/GPT_SoVITS/pretrained_models/chinese-roberta-wwm-ext-large/pytorch_model.bin",
             "/GPT_SoVITS/pretrained_models/chinese-roberta-wwm-ext-large/tokenizer.json"})
        writeFixture(package + file);
    for (const auto &file : QStringList{launcher, "/webui.py", "/config.py", "/runtime/python.exe", inference})
        writeFixture(sameName + file, "Different build");
    writeFixture(data + "/compat-registry-v1.json", "{ broken first-run cache");

    EngineManager engine;
    CompatibilityRegistry registry(data, engine);
    QFile baseline(":/compat/registry-v1.json");
    check(baseline.open(QIODevice::ReadOnly), "Cannot load bundled registry");
    auto updated = QJsonDocument::fromJson(baseline.readAll()).object();
    const auto baselineRevision = updated.value("revision").toInt();
    const auto nextRevision = baselineRevision + 1;
    check(registry.revision() == baselineRevision && registry.inspect(package + launcher).package().isEmpty(),
        "Offline baseline was not used");
    auto packages = updated.value("packages").toArray();
    const QJsonObject entry{{"id", "test-synthesis-only"}, {"name", "Synthesis-only fixture"},
        {"identity_source", "Component test"}, {"adapter", defaultEngineAdapter()},
        {"scripts", QJsonObject{{"go-webui.bat", sha(package + launcher)}, {"webui.py", sha(package + "/webui.py")},
                                 {"GPT_SoVITS/inference_webui_fast.py", sha(package + inference)}}},
        {"capabilities", QJsonArray{
            QJsonObject{{"operation", "slice"}, {"resources", "20261006"}, {"result", "passed"}, {"report", "fixture"},
                        {"scripts", QJsonObject{{"tools/slice_audio.py", sha(package + "/tools/slice_audio.py")}}}},
            QJsonObject{{"operation", "synthesize"}, {"family", "v2"}, {"resources", "20261006"},
                        {"result", "passed"}, {"report", "fixture"}},
            QJsonObject{{"operation", "synthesize"}, {"family", "v2Pro"}, {"resources", "20261006"},
                        {"result", "passed"}, {"report", "fixture"}}}}};
    packages.append(entry);
    updated["revision"] = nextRevision;
    updated["packages"] = packages;
    const auto candidate = QJsonDocument(updated).toJson();
    registry.install(candidate);
    check(registry.revision() == nextRevision && registry.install(candidate).contains(QStringLiteral("已是最新")),
        "Registry revision was not refreshed or recognized on a repeat import");
    const auto unsupportedLegacy = [&](const QString &operation, const QString &family) {
        auto invalidRegistry = updated;
        invalidRegistry["revision"] = nextRevision + 1;
        auto invalidPackages = packages;
        for (qsizetype index = 0; index < invalidPackages.size(); ++index) {
            auto legacy = invalidPackages.at(index).toObject();
            if (legacy.value("adapter") != "0725-cu124")
                continue;
            auto capabilities = legacy.value("capabilities").toArray();
            capabilities.append(QJsonObject{{"operation", operation}, {"family", family}, {"resources", "20261006"},
                                            {"result", "passed"}, {"report", "fixture"}});
            legacy["capabilities"] = capabilities;
            invalidPackages[index] = legacy;
            break;
        }
        invalidRegistry["packages"] = invalidPackages;
        return QJsonDocument(invalidRegistry).toJson();
    };
    for (const auto &invalid : {QByteArray("{ broken"), QByteArray(1024 * 1024 + 1, ' '),
                                QJsonDocument(QJsonObject{{"format", 1}, {"revision", nextRevision + 1}}).toJson(),
                                unsupportedLegacy("train_gpt", "v2"), unsupportedLegacy("synthesize", "v5turbo")}) {
        bool rejected = false;
        try {
            registry.install(invalid);
        } catch (const std::exception &) {
            rejected = true;
        }
        check(rejected && registry.revision() == nextRevision, "An invalid import replaced an accepted revision");
    }
    CompatibilityRegistry reopened(data, engine);
    check(reopened.revision() == nextRevision && reopened.inspect(package + launcher).package() == "test-synthesis-only",
        "A valid revision did not persist across restart");
    {
        const auto previous = QNetworkProxy::applicationProxy();
        const auto restore = qScopeGuard([&] { QNetworkProxy::setApplicationProxy(previous); });
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", 1));
        QSignalSpy result(&reopened, &CompatibilityRegistry::updateFinished);
        reopened.checkOnline();
        check(result.wait(14000) && result.first().first().toString().contains(QStringLiteral("继续使用修订 %1").arg(nextRevision))
            && reopened.revision() == nextRevision, "Network failure did not preserve the last good registry");
    }
    check(reopened.inspect(sameName + launcher).package().isEmpty(), "Directory name created a false match");
    const auto renamed = data + "/first/Renamed build";
    check(QDir().rename(package, renamed), "Cannot rename engine fixture");
    writeFixture(renamed + launcher, "@rem Changed local launch arguments");
    writeFixture(renamed + "/config.py", "webui_port_main = 19874\nwebui_port_infer_tts = 19872\n");
    auto installation = reopened.inspect(renamed + launcher);
    check(installation.package() == "test-synthesis-only" && !installation.identityNote.isEmpty()
        && enginePorts(renamed, installation.adapter).inference == 19872,
        "Renaming or local launcher and port edits invalidated the identity");
    check(installation.families({"synthesize"}) == QStringList{"v2Pro", "v2"}
        && installation.families({"prepare", "train_sovits", "train_gpt"}).isEmpty(),
        "A synthesis-only family was exposed for full training");
    check(QFile::remove(renamed + speaker), "Cannot remove capability-specific asset");
    installation = reopened.inspect(renamed + launcher);
    check(installation.families({"synthesize"}) == QStringList{"v2"}
        && installation.issue("synthesize", "v2Pro").contains(speaker.mid(1)),
        "A missing speaker resource changed unrelated choices or lost its path");
    writeFixture(renamed + speaker);
    check(reopened.inspect(renamed + launcher).families({"synthesize"}) == QStringList{"v2Pro", "v2"},
        "Restoring a resource did not restore availability");
    reopened.recordInferenceIssue(renamed + launcher, installation.fingerprint, "v2", "Recorded inference contract conflict");
    installation = reopened.inspect(renamed + launcher);
    check(installation.families({"synthesize"}) == QStringList{"v2Pro"} && installation.issue("slice").isEmpty(),
        "An observed inference conflict was ignored or disabled other capabilities");
    reopened.clearInterfaceIssues(renamed + launcher);
    check(!reopened.inspect(renamed + launcher).families({"synthesize"}).isEmpty(), "Re-detection did not invalidate the conflict");
    writeFixture(renamed + "/tools/slice_audio.py", "Changed slicer");
    installation = reopened.inspect(renamed + launcher);
    check(installation.issue("slice").contains("tools/slice_audio.py") && !installation.families({"synthesize"}).isEmpty(),
        "A changed operation script affected the wrong capability");
    writeFixture(renamed + "/tools/slice_audio.py");
    writeFixture(renamed + inference, "Changed inference script");
    installation = reopened.inspect(renamed + launcher);
    check(installation.families({"synthesize"}).isEmpty() && installation.issue("slice").isEmpty()
        && installation.issue("synthesize", "v2").contains(inference.mid(1)),
        "The changed inference script was not isolated from package operations");
    writeFixture(renamed + "/webui.py", "Changed main script");
    check(reopened.inspect(renamed + launcher).package().isEmpty(), "An in-place upgrade retained registration");
}

void draftExport(const QDir &root)
{
    using namespace ez::adaptation;
    const auto directory = root.filePath("draft-sample");
    const auto output = root.filePath("exports");
    check(QDir().mkpath(output), "Cannot make the test export directory");
    const QString inference = "GPT_SoVITS/inference_webui_fast.py";
    for (const auto &path : QStringList{"go-webui.bat", "webui.py", inference})
        writeFixture(directory + '/' + path, path.toUtf8());
    Capture capture;
    capture.directory = directory;
    capture.adapter = defaultEngineAdapter();
    capture.fingerprint = engineFingerprint(directory);
    capture.identity = {{"webui.py", engineScriptDigest(directory, "webui.py")},
        {inference, engineScriptDigest(directory, inference)}};
    capture.checks = {{"start", {}, {}, {}, {}}, {"synthesize", "v2", {}, QStringLiteral("快速推理参数变化"), {}}};
    auto baseline = loadBaseline({});
    Draft draft{"wizard-fixture", "Wizard fixture", "Fixture package archive", baseline,
        {{0, "unverified", {}}, {1, "unverified", {}}}};
    const auto path = exportDraft(capture, draft, output, false);
    auto read = [](const QString &file) {
        QFile source(file);
        check(source.open(QIODevice::ReadOnly), "Cannot read generated compatibility draft");
        return validatedCompatibilityRegistry(source.readAll());
    };
    const auto draftJson = read(path + "/registry-candidate.json");
    const auto entries = draftJson.value("packages").toArray().last().toObject().value("capabilities").toArray();
    check(entries.at(0).toObject().value("result") == "unverified"
        && entries.at(1).toObject().value("result") == "unverified"
        && QFileInfo::exists(path + "/PR-草稿.md"), "A desk check must not mark actual acceptance as passed");
    const auto local = exportDraft(capture, draft, output, true);
    const auto localEntries = read(local + "/registry-local-test.json").value("packages").toArray().last().toObject()
        .value("capabilities").toArray();
    check(localEntries.at(0).toObject().value("result") == "passed"
        && localEntries.at(1).toObject().value("result") == "unverified", "Local candidate enabled an incompatible operation");
    check(!QFileInfo::exists(local + "/PR-草稿.md"), "Local testing artifacts must be separate from the PR draft");
    draft.entries[0].result = "passed";
    bool refused = false;
    try {
        exportDraft(capture, draft, output, false);
    } catch (const std::exception &) {
        refused = true;
    }
    check(refused, "Exported final acceptance without evidence");
    draft.entries[0].evidence = "Observed the standalone startup.";
    const auto accepted = exportDraft(capture, draft, output, false);
    check(accepted != path && read(accepted + "/registry-candidate.json").value("revision").toInt()
        == baseline.value("revision").toInt() + 1, "Draft collision or invalid revision");
}

void liveDraftCapture(const QString &engine, const QString &output)
{
    using namespace ez::adaptation;
    const auto captured = scan(engine, defaultEngineAdapter(), true);
    check(captured.inferenceIssue.isEmpty() && !captured.inference.info.isEmpty(),
        "Could not capture the real quick-inference service");
    Draft draft{"20261006", "20261006 live test fixture", "Independent engine fixture", loadBaseline({}), {}};
    for (int index = 0; index < captured.checks.size(); ++index) {
        const auto &operation = captured.checks.at(index);
        if (operation.operation == "start" || (operation.operation == "synthesize" && operation.family == "v5turbo")) {
            check(operation.issue.isEmpty(), operation.issue);
            draft.entries.append({index, "unverified", {}});
        }
    }
    check(draft.entries.size() == 2, "Missing a required operation in the real interface capture");
    const auto draftPath = exportDraft(captured, draft, output, false);
    const auto localPath = exportDraft(captured, draft, output, true);
    check(QFileInfo::exists(draftPath + "/registry-candidate.json") && QFileInfo::exists(draftPath + "/PR-草稿.md")
        && QFileInfo::exists(localPath + "/capture-local/inference-config.json"),
        "Real capture failed to produce the candidate registry, local evidence or PR draft");
    for (const auto &path : {draftPath + "/registry-candidate.json", localPath + "/registry-local-test.json"}) {
        QTemporaryDir storage(QDir(output).filePath("registry-import-XXXXXX"));
        check(storage.isValid(), "Cannot prepare registry import test");
        EngineManager manager;
        CompatibilityRegistry registry(storage.path(), manager);
        QFile file(path);
        check(file.open(QIODevice::ReadOnly), "Cannot read the generated registry");
        registry.install(file.readAll());
        const auto selected = registry.inspect(captured.directory + "/go-webui.bat");
        check(selected.package() == "20261006" && selected.issue("synthesize", "v5turbo").isEmpty(),
            "Updating an already published build must preserve compatibility while a recheck is pending");
    }
    QTextStream(stdout) << "Real adaptation exports: " << draftPath << " | " << localPath << Qt::endl;
}

void recordings(const QDir &root)
{
    const auto old = root.filePath("old");
    const QString filename = "clip-1.wav";
    for (int i = 1; i <= 10; ++i)
        writeWave(QDir(old).filePath(QString("clip-%1.wav").arg(i)), 16000, 80, qint16(i * 300));
    const auto original = QDir(old).filePath(filename);
    const auto originalSha = sha(original);
    const auto sameName = writeWave(root.filePath("incoming/" + filename), 16000, 80, 7000);
    const auto next = writeWave(root.filePath("incoming/clip-new.wav"), 16000, 80, 8000);
    std::atomic_bool cancelled = false;
    std::atomic_int completed = 0;
    const auto plan = planRecordings({sameName, next, original}, old, {}, {}, {}, "batch-two", cancelled);
    check(plan.error.isEmpty() && plan.files.size() == 12 && plan.additions == 2
        && plan.duplicates.size() == 1 && plan.conflicts.size() == 1, QStringLiteral("Append plan: %1").arg(plan.error));
    const auto newClips = root.filePath("new");
    const auto error = copyRecordings(plan, newClips, {}, {}, cancelled, completed);
    check(error.isEmpty() && completed == 12, QStringLiteral("Append copy: %1").arg(error));
    check(sha(QDir(newClips).filePath(filename)) == originalSha
        && sha(QDir(newClips).filePath("clip-1-2.wav")) == sha(sameName), "Original or collision contents changed");
    QJsonObject groups;
    for (const auto &recording : plan.files)
        groups[recording.name] = recording.batch;
    const auto replace = planRecordings({sameName}, newClips, groups, {}, "batch-two", "batch-three", cancelled);
    check(replace.error.isEmpty() && replace.files.size() == 11 && replace.additions == 1,
        "Replacing the most recent batch changed a previous batch");
    check(!copyRecordings(plan, original, {}, {}, cancelled, completed).isEmpty() && sha(original) == originalSha,
        "Copy failure changed the existing file");
    auto longPlan = plan;
    const auto large = root.filePath("incoming/large.wav");
    QFile largeFile(large);
    check(largeFile.open(QIODevice::WriteOnly) && largeFile.resize(64 * 1024 * 1024), "Cannot size cancellation fixture");
    largeFile.close();
    longPlan.files.insert(1, {large, "large.wav", "large.wav", "batch-two", {}, true});
    completed = 0;
    std::atomic_bool finished = false;
    std::jthread cancelAfterCopy([&] {
        while (completed == 0 && !finished)
            std::this_thread::yield();
        cancelled = true;
    });
    const auto cancelledCopy = copyRecordings(longPlan, root.filePath("cancelled"), {}, {}, cancelled, completed);
    finished = true;
    cancelAfterCopy.join();
    check(!cancelledCopy.isEmpty() && completed >= 1 && completed < longPlan.files.size()
        && sha(original) == originalSha, "Mid-copy cancellation modified old recordings or completed the import");
}

void exports(const QDir &root)
{
    const auto first = writeWave(root.filePath("export/first.wav"), 8000, 100, 4096);
    const auto second = writeWave(root.filePath("export/second.wav"), 24000, 100, -4096);
    const QList<WaveExportItem> items{{first, sha(first), "001_RoleA.wav"}, {second, sha(second), "002_RoleB.wav"}};
    const auto folder = root.filePath("export");
    std::atomic_bool cancelled = false;
    const auto orderedError = exportOrderedWaves(items, folder, {}, cancelled);
    check(orderedError.isEmpty(), orderedError);
    for (const auto &item : items)
        check(sha(QDir(folder).filePath(item.filename)) == item.sha256, "Ordered export checksum changed");
    check(!exportOrderedWaves(items, folder, {}, cancelled).isEmpty(), "Overwrite took place without approval");
    const auto merged = root.filePath("export/combined.wav");
    const auto mergeError = exportMergedWave(items, merged, 125, false, cancelled);
    check(mergeError.isEmpty(), mergeError);
    QFile audio(merged);
    check(audio.open(QIODevice::ReadOnly), "Cannot reopen merged WAV");
    const auto bytes = audio.readAll();
    check(bytes.startsWith("RIFF") && qFromLittleEndian<quint32>(bytes.constData() + 24) == 48000,
        "Unexpected file type or sample rate");
    const auto frames = qFromLittleEndian<quint32>(bytes.constData() + 40) / 2;
    check(qAbs(int(frames) - 15600) <= 48 && bytes.size() == 44 + frames * 2, "Merged duration or byte count differs");
    const auto value = [&](int frame) { return qFromLittleEndian<qint16>(bytes.constData() + 44 + frame * 2); };
    check(value(2400) > 3000 && value(7500) == 0 && value(13500) < -3000, "Merged clip order or silence is wrong");
}

QPushButton *button(QWidget &widget, const QString &text)
{
    for (auto *control : widget.findChildren<QPushButton *>()) {
        if (control->text() == text)
            return control;
    }
    throw std::runtime_error(QStringLiteral("Missing control %1").arg(text).toStdString());
}

QString operateImport(TrainingPanel &panel, const QString &control, const QStringList &files,
                      const QString &previewTitle)
{
    QString result;
    bool localized = false;
    QSet<QString> seen;
    QStringList dialogs;
    QElapsedTimer clock;
    clock.start();
    QTimer driver;
    driver.setInterval(50);
    QObject::connect(&driver, &QTimer::timeout, &panel, [&] {
        auto *widget = QApplication::activeModalWidget();
        if (!widget)
            return;
        const auto description = QString(widget->metaObject()->className()) + " " + widget->windowTitle();
        if (!dialogs.contains(description))
            dialogs.append(description);
        if (clock.elapsed() > 12000) {
            if (auto *dialog = qobject_cast<QDialog *>(widget))
                dialog->reject();
            return;
        }
        if (seen.contains(description))
            return;
        if (auto *dialog = qobject_cast<QFileDialog *>(widget)) {
            auto *input = dialog->findChild<QLineEdit *>("fileNameEdit");
            if (!input)
                return;
            dialog->setDirectory(QFileInfo(files.first()).absolutePath());
            QStringList names;
            for (const auto &file : files)
                names.append(files.size() == 1 ? QFileInfo(file).fileName() : '"' + QFileInfo(file).fileName() + '"');
            input->setFocus();
            input->selectAll();
            QTest::keyClicks(input, names.join(' '));
            seen.insert(description);
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        } else if (auto *box = qobject_cast<QMessageBox *>(widget)) {
            QPushButton *choice = nullptr;
            if (widget->windowTitle() == previewTitle) {
                result = box->text() + '\n' + box->detailedText();
                choice = qobject_cast<QPushButton *>(box->button(QMessageBox::Apply));
                const auto *cancel = box->button(QMessageBox::Cancel);
                localized = choice && choice->text() == QStringLiteral("应用")
                    && cancel && cancel->text() == QStringLiteral("取消");
            } else if (widget->windowTitle() == QStringLiteral("导入录音")) {
                choice = button(*box, QStringLiteral("追加"));
            }
            if (choice) {
                seen.insert(description);
                QMetaObject::invokeMethod(choice, "click", Qt::QueuedConnection);
            }
        }
    });
    driver.start();
    button(panel, control)->click();
    driver.stop();
    if (result.isEmpty()) {
        for (const auto *label : panel.findChildren<QLabel *>()) {
            if (!label->text().isEmpty())
                dialogs.append(label->text());
        }
        check(false, QStringLiteral("No import preview for %1\n%2").arg(control, dialogs.join('\n')));
    }
    check(localized, QStringLiteral("The import preview buttons must be in Chinese"));
    return result;
}

void trainingReview(const QDir &root)
{
    const auto data = root.filePath("training");
    const auto projectKey = QStringLiteral("11111111111111111111111111111111");
    const auto project = data + "/projects/" + projectKey;
    const auto clips = project + "/clips";
    QString labels;
    for (int index = 0; index < 10; ++index) {
        const auto clip = writeWave(QString("%1/clip-%2.wav").arg(clips).arg(index, 2, 10, QChar('0')),
            16000, 600, qint16(500 + index));
        labels += QString("%1|Speaker|en|Reviewed line %2\n").arg(clip).arg(index);
    }
    const auto annotation = project + "/labels.list";
    QFile labelFile(annotation);
    check(labelFile.open(QIODevice::WriteOnly) && labelFile.write(labels.toUtf8()) == labels.toUtf8().size(), "Cannot seed project labels");
    labelFile.close();
    auto database = QSqlDatabase::addDatabase("QSQLITE");
    database.setDatabaseName(data + "/library.sqlite");
    check(database.open(), database.lastError().text());
    initWorkspaceSettings();
    QSqlQuery query;
    check(query.exec("CREATE TABLE engines(id INTEGER PRIMARY KEY, name TEXT, launcher TEXT)"), query.lastError().text());
    check(openWorkspacePaths(data, {}), "Cannot initialize test library");
    check(query.exec("CREATE TABLE projects(id TEXT PRIMARY KEY,name TEXT,state BLOB)"), query.lastError().text());
    query.prepare("INSERT INTO projects VALUES (?,?,?)");
    query.addBindValue(projectKey);
    query.addBindValue("Recording review");
    query.addBindValue(QJsonDocument(storedWorkspacePaths({
        {"id", projectKey}, {"name", "Recording review"}, {"directory", project}, {"clips", clips},
        {"annotation", annotation}, {"speaker", "Speaker"}, {"language", "en"}, {"family", "v5turbo"},
    })).toJson(QJsonDocument::Compact));
    check(query.exec(), query.lastError().text());
    query.finish();
    EngineManager engine;
    TaskScheduler scheduler(data, data + "/outputs", engine);
    TrainingPanel panel(data, scheduler, [] { return QString(); }, [](QJsonObject) {});
    auto *output = panel.findChild<QComboBox *>("clipAudioOutput");
    check(output && output->count() == QMediaDevices::audioOutputs().size() + 1, "Missing startup audio inventory");
    const auto system = QMediaDevices::defaultAudioOutput();
    const auto expected = system.isNull() ? QStringLiteral("无可用音频输出")
        : QStringLiteral("系统默认 · %1").arg(system.description());
    check(output->itemText(0) == expected, "Default device name is absent from the output selector");
    output->addItem("Disconnected test device", QByteArray("fixture-removed"));
    output->setCurrentIndex(output->count() - 1);
    auto *mediaDevices = output->findChild<QMediaDevices *>();
    check(mediaDevices && QMetaObject::invokeMethod(mediaDevices, "audioOutputsChanged"), "Missing new audio inventory signal");
    check(output->currentIndex() == 0 && output->itemText(0) == expected, "Missing device failed to fall back to the system output");
    output->setItemText(0, "Stale label");
    const auto devices = reportedTrainingDevices(QStringLiteral("0\tNVIDIA GeForce RTX 4070 Laptop GPU\n2\tNVIDIA GeForce RTX 4060"));
    check(devices.size() == 2 && devices.value(2) == "NVIDIA GeForce RTX 4060"
        && reportedTrainingDevices(QStringLiteral("0\tCPU CPU训练,较慢")).value(0) == "CPU",
        "Cannot parse engine device names and ordinals");
    auto *device = panel.findChild<QComboBox *>("trainingDevice");
    check(device, "Cannot find the training device selector");
    panel.setDeviceNames(devices);
    check(device->currentText() == devices.value(0) && device->currentData().toString() == "0",
        "Training device must show the model and retain the request ordinal");
    device->setCurrentIndex(device->findData(2));
    panel.setDeviceNames({{0, devices.value(0)}});
    check(device->currentText() == QStringLiteral("原设备不可用") && device->currentData().toString() == "2",
        "An unavailable saved ordinal must survive a device refresh");
    panel.setDeviceNames(devices);
    check(device->currentText() == devices.value(2), "Returning hardware must recover its model name");
    panel.show();
    check(output->itemText(0) == expected, "Opening training did not refresh audio output");
    auto *table = panel.findChild<QTableWidget *>();
    check(table && table->rowCount() == 10, "Cannot load reviewed project");
    const auto newA = writeWave(root.filePath("new-clips/new-a.wav"), 16000, 700, 800);
    const auto newB = writeWave(root.filePath("new-clips/new-b.wav"), 16000, 700, 900);
    operateImport(panel, QStringLiteral("导入已切片录音"), {newA, newB}, QStringLiteral("录音导入预览"));
    check(table->rowCount() == 12, "Appending two clips did not retain all ten reviewed clips.");
    for (int index = 0; index < 10; ++index) {
        check(table->item(index, 2)->text() == QStringLiteral("已校对")
            && table->item(index, 3)->text() == QString("Reviewed line %1").arg(index), "A previous review was changed.");
    }
    for (int index = 10; index < 12; ++index)
        check(table->item(index, 2)->text() == QStringLiteral("待校对"), "A new clip was not marked pending.");
    QFile incoming(root.filePath("incoming-annotations.list"));
    check(incoming.open(QIODevice::WriteOnly), "Cannot write incoming annotations fixture");
    incoming.write("new-a.wav|Speaker|en|New line A\nnew-b.wav|Speaker|en|First B\n"
                   "new-b.wav|Speaker|en|Duplicate B\nabsent.wav|Speaker|en|Missing\nmalformed row\n");
    incoming.close();
    const auto report = operateImport(panel, QStringLiteral("导入标注"), {incoming.fileName()}, QStringLiteral("标注导入差异"));
    for (const auto &item : {QStringLiteral("唯一匹配 1 条"), QStringLiteral("音频缺失 1 条"),
                             QStringLiteral("重复片段 1 条"), QStringLiteral("格式错误 1 行")})
        check(report.contains(item), report);
    check(table->item(10, 3)->text() == "New line A" && table->item(11, 3)->text().isEmpty(),
        "The import changed an ambiguous row.");
    table->setCurrentCell(10, 0);
    table->setFocus();
    if (!QMediaDevices::defaultAudioOutput().isNull()) {
        auto *player = panel.findChild<QMediaPlayer *>();
        player->audioOutput()->setMuted(true);
        QTest::keyClick(table, Qt::Key_Space);
        check(QTest::qWaitFor([&] { return player->playbackState() == QMediaPlayer::PlayingState; }, 3000),
            "Space did not start clip playback.");
        QTest::keyClick(table, Qt::Key_Space);
        check(player->playbackState() == QMediaPlayer::PausedState, "Space did not pause clip playback.");
    }
    table->setCurrentCell(10, 3);
    table->editItem(table->item(10, 3));
    QLineEdit *editor = nullptr;
    check(QTest::qWaitFor([&] {
        for (auto *candidate : table->findChildren<QLineEdit *>()) {
            if (candidate->isVisible()) {
                editor = candidate;
                return true;
            }
        }
        return false;
    }, 1000), "Cannot open the subtitle editor.");
    editor->setFocus();
    editor->selectAll();
    QTest::keyClicks(editor, "Edited with spaces");
    QTest::keyClick(editor, Qt::Key_Return, Qt::ControlModifier);
    check(table->currentRow() == 11 && table->item(10, 3)->text() == "Edited with spaces"
        && table->item(10, 2)->text() == QStringLiteral("已校对"), "Saving the edit did not advance.");
    output->setItemText(0, "Stale dropdown label");
    output->showPopup();
    output->hidePopup();
    check(output->itemText(0) == expected, "Opening the audio menu did not refresh the default device label");
    {
        const auto gpt = project + "/fixture.ckpt";
        const auto sovits = project + "/fixture.pth";
        writeFixture(gpt, "GPT fixture");
        writeFixture(sovits, "SoVITS fixture");
        SynthesisPanel synthesis(data, scheduler, [] { return QString(); });
        synthesis.show();
        QTimer confirm;
        confirm.setInterval(20);
        QObject::connect(&confirm, &QTimer::timeout, &synthesis, [&] {
            auto *modal = QApplication::activeModalWidget();
            if (modal) {
                confirm.stop();
                button(*modal, QStringLiteral("保存声音"))->click();
            }
        });
        confirm.start();
        synthesis.importVoice({{"id", projectKey}, {"name", "Finished voice"}, {"family", "v5turbo"}, {"gpt", gpt},
            {"sovits", sovits}, {"reference", clips + "/clip-00.wav"}, {"prompt", "Reviewed line 0"},
            {"prompt_language", QStringLiteral("英文")}});
        check(QTest::qWaitFor([&] { return !synthesis.importing(); }, 4000), "Saving the trained voice did not finish");
        QSqlQuery stored("SELECT settings FROM voices WHERE name='Finished voice'");
        check(stored.next() && readWorkspaceRecord(stored.value(0).toByteArray()).value("project_id") == projectKey,
            "Trained voice was not saved with its project");
        QSqlQuery jobs("SELECT COUNT(*) FROM jobs");
        check(jobs.next() && jobs.value(0).toInt() == 0, "Saving a trained voice must not start a test synthesis");
    }
}

void scriptReview(const QDir &root)
{
    const auto data = root.filePath("training");
    QSqlQuery query;
    check(query.exec("CREATE TABLE IF NOT EXISTS voices(id TEXT PRIMARY KEY,name TEXT,settings BLOB)"), query.lastError().text());
    check(query.exec("DELETE FROM voices WHERE name='Finished voice'"), query.lastError().text());
    const QString voiceA = "a1111111-1111-1111-1111-111111111111";
    const QString voiceB = "b2222222-2222-2222-2222-222222222222";
    const QStringList voices{voiceA, voiceB, voiceA};
    query.prepare("INSERT INTO voices VALUES(?,?,?)");
    for (const auto &voice : {voiceA, voiceB}) {
        query.bindValue(0, voice);
        query.bindValue(1, voice == voiceA ? "A" : "B");
        query.bindValue(2, QByteArray(R"({"family":"v5turbo","default_language":"英文"})"));
        check(query.exec(), query.lastError().text());
    }
    const auto fake = root.filePath("offline-engine/go-webui.bat");
    check(QDir().mkpath(QFileInfo(fake).path()), "Cannot create fixture engine directory.");
    QFile launcher(fake);
    check(launcher.open(QIODevice::WriteOnly), "Cannot create fixture launcher.");
    launcher.write("@exit /b 1\r\n");
    launcher.close();
    EngineManager engine;
    TaskScheduler scheduler(data, data + "/outputs", engine);
    QJsonArray script;
    QStringList lineIds;
    query.prepare("INSERT INTO jobs(id,created,launcher,operation,state,request,result) VALUES(?,?,?,?,?,?,?)");
    for (int index = 0; index < 3; ++index) {
        const auto lineId = QString("line-%1").arg(index);
        lineIds.append(lineId);
        script.append(QJsonObject{{"id", lineId}, {"revision", "initial"}, {"voice", voices.at(index)},
                                  {"text", QString("Original line %1").arg(index)}});
        const auto wave = writeWave(root.filePath(QString("script-audio/%1.wav").arg(index)), 24000, 100, qint16(100 + index));
        const QJsonObject request{{"line_id", lineId}, {"line_revision", "initial"}, {"voice_id", voices.at(index)}};
        const QJsonObject result{{"audio", wave}, {"sha256", sha(wave)}};
        query.bindValue(0, QString("fixture-job-%1").arg(index));
        query.bindValue(1, "2026-01-01T00:00:00.000Z");
        query.bindValue(2, fake);
        query.bindValue(3, "synthesize");
        query.bindValue(4, "succeeded");
        query.bindValue(5, QJsonDocument(request).toJson(QJsonDocument::Compact));
        query.bindValue(6, QJsonDocument(result).toJson(QJsonDocument::Compact));
        check(query.exec(), query.lastError().text());
    }
    query.finish();
    setWorkspaceSetting("scriptLines", QString::fromUtf8(QJsonDocument(script).toJson(QJsonDocument::Compact)));
    QWidget parent;
    QString error;
    QTimer::singleShot(0, &parent, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) {
            error = "Script dialogue did not open.";
            return;
        }
        try {
            auto *table = dialog->findChild<QTableWidget *>();
            check(table && table->rowCount() == 3 && table->item(0, 1)->text() == "A"
                && table->item(1, 1)->text() == "B" && table->item(2, 1)->text() == "A"
                && table->item(1, 3)->text() == QStringLiteral("已生成"), "Script voices or results were not loaded.");
            table->item(1, 2)->setText("Edited middle line");
            check(table->item(1, 3)->text() == QStringLiteral("修改后待重做"), "Editing did not invalidate the old result.");
            table->selectRow(1);
            button(*dialog, QStringLiteral("只重做所选"))->click();
        } catch (const std::exception &failure) {
            error = QString::fromUtf8(failure.what());
        }
        dialog->reject();
    });
    editScriptLines(&parent, scheduler, fake, voiceA, {}, [](const QString &) {});
    check(error.isEmpty(), error);
    check(query.exec("SELECT request FROM jobs ORDER BY rowid"), query.lastError().text());
    int records = 0;
    QJsonObject latest;
    while (query.next()) {
        latest = QJsonDocument::fromJson(query.value(0).toByteArray()).object();
        ++records;
    }
    check(records == 4 && latest.value("line_id").toString() == lineIds.at(1)
        && latest.value("line_revision").toString() != "initial" && latest.value("line_order").toInt() == 2
        && latest.value("text").toString() == "Edited middle line" && latest.value("voice_id").toString() == voiceB,
        "The selected-row retry changed another line or lost the edited snapshot.");
    check(QFileInfo(root.filePath("script-audio/1.wav")).isFile(), "Editing removed the previous revision's WAV.");
}

}

int main(int argc, char **argv)
{
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    try {
        const auto arguments = app.arguments();
        if (arguments.size() == 4 && arguments.at(1) == "--capture-adaptation") {
            liveDraftCapture(arguments.at(2), arguments.at(3));
            return 0;
        }
        if (arguments.size() != 2)
            return 2;
        QTemporaryDir scratch(QDir(arguments.at(1)).filePath("workflow-components-XXXXXX"));
        check(scratch.isValid(), "Cannot create temporary project fixtures");
        const QDir root(scratch.path());
        engineCompatibility(root);
        draftExport(root);
        recordings(root);
        exports(root);
        trainingReview(root);
        scriptReview(root);
        comboPopupPlacement();
        QTextStream(stdout) << "Compatibility registry, recording append, failed/cancelled copy, Qt label review, script revision, mixed-rate WAV export and dropdown placement passed."
                            << Qt::endl;
    } catch (const std::exception &error) {
        QTextStream(stderr) << error.what() << Qt::endl;
        return 1;
    }
    return 0;
}
