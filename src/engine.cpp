#include "engine.h"
#include "resources.h"

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

#include <QCoreApplication>
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
#include <QProcessEnvironment>
#include <QStorageInfo>
#include <QtConcurrentRun>
#include <stdexcept>
#include <vector>

namespace ez {
namespace {

constexpr quint16 mainPort = 9874;
constexpr quint16 inferencePort = 9872;

[[noreturn]] void fail(const QString &message)
{
    throw std::runtime_error(message.toStdString());
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

EngineProbe probeApi()
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
    EngineProbe result;
    result.responded = true;
    if (config.value("version").toString() != "4.44.1") {
        result.error = QStringLiteral("WebUI 响应成功，但 Gradio 版本与 20261006 适配契约不符");
        return result;
    }
    for (const auto &component : config.value("components").toArray()) {
        const auto props = component.toObject().value("props").toObject();
        const auto label = props.value("label").toString();
        if (label == QStringLiteral("显卡信息") || label.compare("GPU Information", Qt::CaseInsensitive) == 0) {
            result.devices = props.value("value").toString().trimmed();
            break;
        }
    }
    const QMap<QString, QMap<QString, int>> expected{
        {"synthesize", {{"switch_version", 1}, {"change_choices", 0}, {"change_tts_inference", 6},
                        {"change_tts_inference_1", 6}}},
        {"slice", {{"open_slice", 10}}},
        {"prepare", {{"switch_version", 1}, {"open1abc", 10}}},
        {"train_sovits", {{"switch_version", 1}, {"open1Ba", 13}}},
        {"train_gpt", {{"switch_version", 1}, {"open1Bb", 9}}},
    };
    QMap<QString, int> published;
    for (const auto &dependency : config.value("dependencies").toArray()) {
        const auto endpoint = dependency.toObject();
        const auto name = endpoint.value("api_name").toString();
        const auto inputs = endpoint.value("inputs");
        if (!name.isEmpty())
            published.insert(name, !inputs.isArray() || published.contains(name) ? -1 : inputs.toArray().size());
    }
    for (auto operation = expected.cbegin(); operation != expected.cend(); ++operation) {
        QStringList missing;
        for (auto method = operation.value().cbegin(); method != operation.value().cend(); ++method) {
            if (published.value(method.key(), -1) != method.value())
                missing << method.key();
        }
        if (!missing.isEmpty())
            result.unavailable.insert(operation.key(), QStringLiteral("%1接口缺失或参数已变化（%2）。请检查整合包版本。")
                .arg(operationLabel(operation.key()), missing.join(QStringLiteral("、"))));
    }
    return result;
}

}

QString checkedPath(const QString &path)
{
    const QString input = QDir::fromNativeSeparators(path);
    const QString absolute = QDir::cleanPath(input);
    if (absolute.size() < 3 || !absolute[0].isLetter() || absolute.mid(1, 2) != ":/"
        || input.contains("//") || input.mid(2).contains(':')) {
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
    if (info.fileName().compare("go-webui.bat", Qt::CaseInsensitive) != 0 || !info.isFile()) {
        fail(QStringLiteral("请选择已安装的 GPT-SoVITS 中的 go-webui.bat"));
    }
    checkedPath(root + "/TEMP");
    checkedPath(root + "/logs");
    return root;
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
    connect(&probe_, &QFutureWatcher<EngineProbe>::finished, this, [this] {
        if (!process_ || ready_)
            return;
        const auto result = probe_.result();
        if (!result.responded)
            return;
        if (!result.error.isEmpty() || !ownsListener(mainPort)) {
            failInstance(result.error.isEmpty() ? QStringLiteral("引擎监听进程已变化") : result.error);
            return;
        }
        unavailable_ = result.unavailable;
        devices_ = result.devices;
        ready_ = true;
        QStringList limited;
        for (const auto &operation : unavailable_.keys())
            limited << operationLabel(operation);
        emit statusChanged(limited.isEmpty() ? QStringLiteral("引擎可以使用 · %1").arg(root_)
            : QStringLiteral("引擎已连接 · %1暂不可用。可在工作台检查具体接口。").arg(limited.join(QStringLiteral("、"))));
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
    const auto issue = resourceIssue(dir, "start", {});
    if (!issue.isEmpty())
        fail(issue);
    for (auto port : {mainPort, inferencePort}) {
        if (const auto pid = listener(port))
            fail(QStringLiteral("端口 %1 已被进程 %2 占用").arg(port).arg(pid));
    }
    const auto log = QDir::toNativeSeparators(checkedPath(logPath));
    const QStorageInfo disk(QFileInfo(log).absolutePath());
    if (disk.isReady() && (disk.isReadOnly() || disk.bytesAvailable() == 0))
        fail(QStringLiteral("数据盘不可写或空间已满，请释放空间或更换可写目录"));
    // ONNX Runtime GPU 1.18.0 throws on missing CUDA DLLs when CUDA_PATH is set.
    // Clearing it in the child shell allows G2PW to fall back to CPU.
    const auto ortGpuMetadata = checkedPath(dir + "/runtime/Lib/site-packages/onnxruntime_gpu-1.18.0.dist-info");
    std::wstring command = QFileInfo(ortGpuMetadata).isDir()
        ? L"cmd.exe /D /Q /V:OFF /C set CUDA_PATH=& .\\go-webui.bat"
        : L"cmd.exe /D /Q /V:OFF /C .\\go-webui.bat";
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
    wchar_t system[MAX_PATH]{};
    GetSystemDirectoryW(system, MAX_PATH);
    const std::wstring cmd = std::wstring(system) + L"\\cmd.exe";
    const auto cwd = QDir::toNativeSeparators(dir);
    // Both Gradio servers inherit this browser command through the launcher.
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("BROWSER", QStringLiteral("\"%1\" %2 %s")
        .arg(QCoreApplication::applicationFilePath(), QString::fromLatin1(engineBrowserNotification)));
    auto variables = environment.toStringList();
    variables.sort(Qt::CaseInsensitive);
    auto childEnvironment = variables.join(QChar(u'\0'));
    childEnvironment.append(QChar(u'\0'));
    const bool launched = jobReady && input != INVALID_HANDLE_VALUE
        && CreateProcessW(cmd.c_str(), command.data(), nullptr, nullptr, TRUE,
                          CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                          childEnvironment.data(), reinterpret_cast<LPCWSTR>(cwd.utf16()), &startup, &created);
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
    unavailable_.clear();
    emit readinessChanged(false);
    emit statusChanged(QStringLiteral("引擎未启动"));
}

void EngineManager::poll()
{
    if (!process_)
        return;
    if (WaitForSingleObject(process_->handle, 0) == WAIT_OBJECT_0 || (!ready_ && ++ticks_ > 240)) {
        failInstance(QStringLiteral("引擎启动超时或已经退出。查看日志：%1").arg(logPath_));
        return;
    }
    if (ready_ || probe_.isRunning())
        return;
    if (ownsListener(mainPort))
        probe_.setFuture(QtConcurrent::run(probeApi));
}

void EngineManager::failInstance(const QString &message)
{
    emit instanceFailed(root_ + "/go-webui.bat", message);
    stop();
    emit statusChanged(message);
}

bool EngineManager::ready() const { return ready_; }
bool EngineManager::active() const { return bool(process_); }
QString EngineManager::root() const { return root_; }

QString EngineManager::interfaceIssue(const QString &operation) const
{
    return unavailable_.value(operation);
}

QMap<QString, QString> EngineManager::interfaceIssues() const
{
    return unavailable_;
}

QString EngineManager::devices() const { return ready_ ? devices_ : QString(); }

quint32 EngineManager::webuiPid() const
{
    const auto pid = listener(mainPort);
    if (!ready_ || !pid || !process_ || !process_->owns(pid))
        fail(QStringLiteral("引擎监听进程已变化，请重启引擎"));
    return pid;
}

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
