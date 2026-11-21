#include "engine.h"

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QtConcurrentRun>
#include <stdexcept>
#include <vector>

namespace ez {
namespace {

const QString protectedRoot = QStringLiteral("E:/GPT-SoVITS-v5-20261005/GPT-SoVITS-v5-20261006");
constexpr quint16 mainPort = 9874;
constexpr quint16 inferencePort = 9872;

[[noreturn]] void fail(const QString &message)
{
    throw std::runtime_error(message.toStdString());
}

bool inside(const QString &path, const QString &root)
{
    return path.compare(root, Qt::CaseInsensitive) == 0
        || path.startsWith(root.endsWith('/') ? root : root + '/', Qt::CaseInsensitive);
}

DWORD listener(quint16 port)
{
    DWORD size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0);
    std::vector<BYTE> table(size);
    if (size == 0 || GetExtendedTcpTable(table.data(), &size, FALSE, AF_INET,
                                       TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) {
        fail(QStringLiteral("无法检查本机 TCP 端口"));
    }
    const auto *rows = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID *>(table.data());
    for (DWORD index = 0; index < rows->dwNumEntries; ++index) {
        if (ntohs(static_cast<u_short>(rows->table[index].dwLocalPort)) == port)
            return rows->table[index].dwOwningPid;
    }
    return 0;
}

QString probeApi()
{
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:9874/config")));
    request.setTransferTimeout(2500);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    auto *reply = network.get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!reply->isFinished())
        loop.exec();
    if (reply->error() != QNetworkReply::NoError)
        return {};
    const auto config = QJsonDocument::fromJson(reply->readAll()).object();
    if (config.value("version").toString() != "4.44.1")
        return QStringLiteral("WebUI 响应成功，但 Gradio 版本与 20261006 适配契约不符");
    const QMap<QString, int> expected{
        {"change_tts_inference", 6}, {"open_slice", 10},
        {"open1abc", 10}, {"open1Ba", 13}, {"open1Bb", 9}, {"switch_version", 1}
    };
    QMap<QString, int> published;
    for (const auto &dependency : config.value("dependencies").toArray()) {
        const auto endpoint = dependency.toObject();
        published.insert(endpoint.value("api_name").toString(), endpoint.value("inputs").toArray().size());
    }
    for (auto entry = expected.cbegin(); entry != expected.cend(); ++entry) {
        if (published.value(entry.key(), -1) != entry.value())
            return QStringLiteral("缺少适配接口 %1 或接口参数已变化").arg(entry.key());
    }
    return QStringLiteral("ready");
}

}

QString checkedPath(const QString &path)
{
    const QString input = QDir::fromNativeSeparators(path);
    const QString absolute = QDir::cleanPath(input);
    if (absolute.size() < 3 || !absolute[0].isLetter() || absolute.mid(1, 2) != ":/"
        || input.contains("//") || input.mid(2).contains(':') || inside(absolute, protectedRoot)) {
        fail(QStringLiteral("路径不可使用：%1").arg(path));
    }
    wchar_t device[32768]{};
    if (!QueryDosDeviceW(reinterpret_cast<LPCWSTR>(absolute.left(2).utf16()), device, 32768)
        || QString::fromWCharArray(device).startsWith("\\??\\")) {
        fail(QStringLiteral("请选择实际磁盘路径：%1").arg(path));
    }
    QString current = absolute.left(3);
    for (const auto &part : absolute.mid(3).split('/', Qt::SkipEmptyParts)) {
        if (part.endsWith('.') || part.endsWith(' ') || part.contains('~'))
            fail(QStringLiteral("路径包含 Windows 别名或尾随字符：%1").arg(path));
        if (!current.endsWith('/'))
            current += '/';
        current += part;
        const auto attributes =
            GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(current).utf16()));
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                fail(QStringLiteral("无法验证路径：%1（%2）").arg(current).arg(error));
        } else if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            fail(QStringLiteral("路径包含目录联接或符号链接：%1").arg(current));
        }
    }
    return absolute;
}

QString engineRoot(const QString &launcher)
{
    const QFileInfo info(checkedPath(launcher));
    const auto root = info.absolutePath();
    if (info.fileName().compare("go-webui.bat", Qt::CaseInsensitive) != 0 || !info.isFile()
        || inside(protectedRoot, root) || !QFileInfo(checkedPath(root + "/runtime/python.exe")).isFile()
        || !QFileInfo(checkedPath(root + "/webui.py")).isFile()) {
        fail(QStringLiteral("请选择独立 GPT-SoVITS Windows 整合包中的 go-webui.bat"));
    }
    checkedPath(root + "/TEMP");
    checkedPath(root + "/logs");
    return root;
}

QString checkedWorkspaceRoot(const QString &path)
{
    const auto absolute = checkedPath(path);
    if (inside(protectedRoot, absolute))
        fail(QStringLiteral("请选择独立的数据目录，当前位置包含受保护的引擎安装目录"));
    return absolute;
}

QStringList inspectEngine(const QString &launcher, const QString &family)
{
    const auto root = engineRoot(launcher);
    const QString models = "GPT_SoVITS/pretrained_models/";
    const bool v5 = family == "v5dev" || family == "v5turbo";
    const QString sovits = v5 ? "gsv-v5-pretrained/s2G" + family + ".pth"
                              : family == "v2" ? "gsv-v2final-pretrained/s2G2333k.pth"
                                               : "v2Pro/s2G" + family + ".pth";
    const QString gpt = family == "v2" ? "gsv-v2final-pretrained/s1bert25hz-5kh-longer-epoch=12-step=369668.ckpt"
                                        : "s1v3.ckpt";
    QStringList report;
    const auto check = [&](const QString &name, const QStringList &resources) {
        QStringList missing;
        for (const auto &resource : resources) {
            QFile file(checkedPath(root + '/' + resource));
            if (!file.open(QIODevice::ReadOnly) || file.size() <= 0)
                missing.append(resource);
        }
        report.append(missing.isEmpty() ? name + QStringLiteral("：文件可读")
            : name + QStringLiteral("：缺少或无法读取\n  ") + missing.join("\n  "));
    };
    check(QStringLiteral("启动"), {"go-webui.bat", "runtime/python.exe", "webui.py", "config.py"});
    auto synthesis = QStringList{"GPT_SoVITS/inference_webui_fast.py",
        models + "chinese-roberta-wwm-ext-large/config.json", models + "chinese-hubert-base/config.json",
        models + sovits};
    if (v5)
        synthesis.append(models + "gsv-v5-pretrained/vocoder.pth");
    check(QStringLiteral("合成"), synthesis);
    auto training = QStringList{"tools/slice_audio.py", "GPT_SoVITS/prepare_datasets/1-get-text.py",
        "GPT_SoVITS/prepare_datasets/2-get-hubert-wav32k.py", "GPT_SoVITS/prepare_datasets/3-get-semantic.py",
        "GPT_SoVITS/s1_train.py", v5 ? "GPT_SoVITS/s2_train_v3_lora.py" : "GPT_SoVITS/s2_train.py",
        models + gpt, models + sovits};
    if (!v5) {
        auto discriminator = sovits;
        discriminator.replace("/s2G", "/s2D");
        training.append(models + discriminator);
    }
    check(QStringLiteral("训练"), training);
    const QString paraformer = "tools/asr/models/speech_paraformer-large_asr_nat-zh-cn-16k-common-vocab8404-pytorch";
    const QString vad = "tools/asr/models/speech_fsmn_vad_zh-cn-16k-common-pytorch";
    const QString punctuation = "tools/asr/models/punc_ct-transformer_zh-cn-common-vocab272727-pytorch";
    check(QStringLiteral("中文自动转写"), {paraformer + "/config.yaml", paraformer + "/model.pt",
        vad + "/config.yaml", vad + "/model.pt", punctuation + "/config.yaml", punctuation + "/model.pt"});
    return report;
}

struct EngineManager::Process {
    HANDLE job = nullptr;
    HANDLE handle = nullptr;

    ~Process()
    {
        if (job)
            CloseHandle(job);
        if (handle)
            CloseHandle(handle);
    }

    bool owns(DWORD pid) const
    {
        const auto candidate = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!candidate)
            return false;
        BOOL owned = FALSE;
        IsProcessInJob(candidate, job, &owned);
        CloseHandle(candidate);
        return owned;
    }
};

EngineManager::EngineManager(QObject *parent) : QObject(parent)
{
    timer_.setInterval(750);
    connect(&timer_, &QTimer::timeout, this, &EngineManager::poll);
    connect(&probe_, &QFutureWatcher<QString>::finished, this, [this] {
        if (!process_ || ready_)
            return;
        const auto result = probe_.result();
        if (result.isEmpty())
            return;
        if (result != "ready" || !ownsListener(mainPort)) {
            stop();
            emit statusChanged(result == "ready" ? QStringLiteral("引擎监听进程已变化") : result);
            return;
        }
        ready_ = true;
        emit statusChanged(QStringLiteral("引擎可以使用 · %1").arg(root_));
        emit readinessChanged(true);
    });
}

EngineManager::~EngineManager()
{
    timer_.stop();
    process_.reset();
    probe_.waitForFinished();
}

void EngineManager::start(const QString &launcher, const QString &logPath)
{
    if (process_ || probe_.isRunning())
        fail(QStringLiteral("已有引擎正在启动或运行"));
    const auto dir = engineRoot(launcher);
    for (auto port : {mainPort, inferencePort}) {
        if (const auto pid = listener(port))
            fail(QStringLiteral("端口 %1 已被进程 %2 占用").arg(port).arg(pid));
    }
    const auto log = QDir::toNativeSeparators(checkedPath(logPath));
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const HANDLE output = CreateFileW(reinterpret_cast<LPCWSTR>(log.utf16()), GENERIC_WRITE,
        FILE_SHARE_READ, &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output == INVALID_HANDLE_VALUE)
        fail(QStringLiteral("无法创建启动日志：%1").arg(logPath));
    const HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    auto candidate = std::make_unique<Process>();
    candidate->job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    const bool jobReady = candidate->job && SetInformationJobObject(candidate->job,
        JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input;
    startup.hStdOutput = startup.hStdError = output;
    PROCESS_INFORMATION created{};
    wchar_t command[] = L"cmd.exe /D /Q /V:OFF /C .\\go-webui.bat";
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring cmd = std::wstring(system) + L"\\cmd.exe";
    const auto cwd = QDir::toNativeSeparators(dir);
    const bool launched = jobReady && input != INVALID_HANDLE_VALUE
        && CreateProcessW(cmd.c_str(), command, nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                          nullptr, reinterpret_cast<LPCWSTR>(cwd.utf16()), &startup, &created);
    const auto launchError = GetLastError();
    CloseHandle(output);
    if (input != INVALID_HANDLE_VALUE)
        CloseHandle(input);
    if (!launched)
        fail(QStringLiteral("无法启动受管引擎进程（%1）").arg(launchError));
    candidate->handle = created.hProcess;
    if (!AssignProcessToJobObject(candidate->job, created.hProcess)
        || ResumeThread(created.hThread) == static_cast<DWORD>(-1)) {
        TerminateProcess(created.hProcess, 1);
        CloseHandle(created.hThread);
        fail(QStringLiteral("无法接管引擎进程树"));
    }
    CloseHandle(created.hThread);
    process_ = std::move(candidate);
    root_ = dir;
    logPath_ = logPath;
    ticks_ = 0;
    timer_.start();
    emit statusChanged(QStringLiteral("正在加载引擎…"));
}

void EngineManager::stop()
{
    timer_.stop();
    process_.reset();
    ready_ = false;
    emit readinessChanged(false);
    emit statusChanged(QStringLiteral("引擎未启动"));
}

void EngineManager::poll()
{
    if (!process_)
        return;
    if (WaitForSingleObject(process_->handle, 0) == WAIT_OBJECT_0 || (!ready_ && ++ticks_ > 240)) {
        stop();
        emit statusChanged(QStringLiteral("引擎启动超时或已经退出。查看日志：%1").arg(logPath_));
        return;
    }
    if (ready_ || probe_.isRunning())
        return;
    if (ownsListener(mainPort))
        probe_.setFuture(QtConcurrent::run(probeApi));
}

bool EngineManager::ready() const { return ready_; }
bool EngineManager::active() const { return bool(process_); }
QString EngineManager::root() const { return root_; }

bool EngineManager::ownsListener(quint16 port) const
{
    try {
        const auto pid = listener(port);
        return process_ && pid && process_->owns(pid);
    } catch (const std::exception &) {
        return false;
    }
}

}
