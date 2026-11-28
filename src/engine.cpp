#include "engine.h"
#include "compatibility.h"
#include "resources.h"

#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>

#include <QCoreApplication>
#include <QCryptographicHash>
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
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStorageInfo>
#include <QtConcurrentRun>
#include <stdexcept>
#include <string>
#include <vector>

namespace ez {
namespace {

[[noreturn]] void fail(const QString &message)
{
    throw std::runtime_error(message.toStdString());
}

QMap<int, QString> localCudaDevices()
{
    const auto library = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library)
        return {};
    const auto unload = qScopeGuard([library] { FreeLibrary(library); });
    const auto init = reinterpret_cast<int (WINAPI *)(unsigned int)>(GetProcAddress(library, "cuInit"));
    const auto count = reinterpret_cast<int (WINAPI *)(int *)>(GetProcAddress(library, "cuDeviceGetCount"));
    const auto device = reinterpret_cast<int (WINAPI *)(int *, int)>(GetProcAddress(library, "cuDeviceGet"));
    const auto name = reinterpret_cast<int (WINAPI *)(char *, int, int)>(GetProcAddress(library, "cuDeviceGetName"));
    int total = 0;
    if (!init || !count || !device || !name || init(0) != 0 || count(&total) != 0)
        return {};

    QMap<int, QString> result;
    for (int index = 0; index < qMin(total, 100); ++index) {
        int handle = 0;
        char model[256]{};
        if (device(&handle, index) == 0 && name(model, int(sizeof(model)), handle) == 0 && model[0])
            result.insert(index, QString::fromUtf8(model));
    }
    return result;
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

QJsonObject fetchApi(quint16 port, const QString &path)
{
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    QNetworkRequest request(QUrl(QStringLiteral("http://127.0.0.1:%1/%2").arg(port).arg(path)));
    request.setTransferTimeout(2500);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    auto *reply = network.get(request);
    QTimer::singleShot(5000, reply, &QNetworkReply::abort);
    constexpr qsizetype limit = 4 * 1024 * 1024;
    QByteArray body;
    bool oversized = false;
    const auto read = [&] {
        if (reply->bytesAvailable() > limit - body.size()) {
            oversized = true;
            reply->abort();
        } else {
            body += reply->readAll();
        }
    };
    QObject::connect(reply, &QNetworkReply::readyRead, reply, read);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!reply->isFinished())
        loop.exec();
    read();
    if (oversized || reply->error() != QNetworkReply::NoError
        || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200)
        return {};
    return QJsonDocument::fromJson(body).object();
}

QString interfaceChange(const QString &method, const QJsonObject &expected, const QJsonArray &dependencies,
                        const QMap<int, QJsonObject> &components, const QJsonObject &info)
{
    QJsonArray inputIds;
    int matches = 0;
    for (const auto &value : dependencies) {
        const auto dependency = value.toObject();
        if (dependency.value("api_name").toString() == method) {
            inputIds = dependency.value("inputs").toArray();
            ++matches;
        }
    }
    if (matches != 1)
        return method + QStringLiteral("（接口缺失或重名）");
    const auto descriptor = info.value('/' + method).toObject();
    const auto parameters = descriptor.value("parameters").toArray();
    const auto inputs = expected.value("inputs").toArray();
    if (descriptor.isEmpty() || inputIds.size() != inputs.size() || parameters.size() != inputs.size())
        return method + QStringLiteral("（入参无法核对）");
    for (qsizetype index = 0; index < inputs.size(); ++index) {
        const auto parameter = parameters.at(index).toObject();
        const auto component = components.value(inputIds.at(index).toInt(-1));
        const QJsonArray observed{parameter.value("parameter_name"), parameter.value("component"),
            parameter.value("type").toObject().value("type"),
            component.value("props").toObject().value("label").toString()};
        if (observed != inputs.at(index).toArray()
            || component.value("type").toString().compare(parameter.value("component").toString(), Qt::CaseInsensitive) != 0)
            return QStringLiteral("%1（第 %2 项入参的顺序、类型或控件已变化）").arg(method).arg(index + 1);
    }
    QJsonArray returns;
    for (const auto &value : descriptor.value("returns").toArray()) {
        const auto item = value.toObject();
        returns.append(QJsonArray{item.value("component"), item.value("type").toObject().value("type")});
    }
    if (returns != expected.value("returns").toArray())
        return method + QStringLiteral("（返回结构已变化）");
    return {};
}

EngineProbe probeApi(quint16 port, QJsonObject contract)
{
    const auto config = fetchApi(port, "config");
    if (config.isEmpty())
        return {};
    EngineProbe result;
    result.responded = true;
    result.gradio = config.value("version").toString();
    if (!config.value("dependencies").isArray() || result.gradio.isEmpty()) {
        result.error = QStringLiteral("引擎未提供可识别的 Gradio 配置");
        return result;
    }
    QMap<int, QJsonObject> components;
    for (const auto &component : config.value("components").toArray()) {
        components.insert(component.toObject().value("id").toInt(-1), component.toObject());
        const auto props = component.toObject().value("props").toObject();
        const auto label = props.value("label").toString();
        if (label == QStringLiteral("显卡信息") || label.compare("GPU Information", Qt::CaseInsensitive) == 0)
            result.devices = props.value("value").toString().trimmed();
    }
    const auto expected = contract.value("operations").toObject();
    const auto methods = contract.value("main").toObject();
    const auto info = fetchApi(port, "info").value("named_endpoints").toObject();
    const auto dependencies = config.value("dependencies").toArray();
    for (auto operation = expected.constBegin(); operation != expected.constEnd(); ++operation) {
        QStringList missing;
        for (const auto &value : operation.value().toArray()) {
            const auto method = value.toString();
            const auto issue = interfaceChange(method, methods.value(method).toObject(), dependencies, components, info);
            if (!issue.isEmpty())
                missing << issue;
        }
        if (!missing.isEmpty())
            result.unavailable.insert(operation.key(), QStringLiteral("%1接口缺失或参数已变化（%2）。请检查整合包版本。")
                .arg(operationLabel(operation.key()), missing.join(QStringLiteral("、"))));
    }
    return result;
}

}

QJsonObject engineContract(const QString &adapter)
{
    const auto selected = adapter.isEmpty() ? defaultEngineAdapter() : adapter;
    QFile file(":/engine/" + adapterContractFile(selected));
    const auto contract = file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
    if (contract.value("id") != selected)
        fail(QStringLiteral("当前程序缺少引擎适配契约：%1").arg(selected));
    return contract;
}

EngineInterfaceSnapshot capturePackageInterface(const QString &directory, quint16 port)
{
    const auto expected = checkedPath(directory + "/runtime/python.exe");
    const auto confirmOwner = [&] {
        const auto pid = listener(port);
        if (!pid)
            fail(QStringLiteral("端口 %1 尚未启动。请先在所选副本中启动对应服务。").arg(port));
        const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process)
            fail(QStringLiteral("无法检查端口 %1 的进程归属。").arg(port));
        const auto close = qScopeGuard([process] { CloseHandle(process); });
        std::wstring image(32768, L'\0');
        auto size = static_cast<DWORD>(image.size());
        if (!QueryFullProcessImageNameW(process, 0, image.data(), &size)
            || QDir::fromNativeSeparators(QString::fromWCharArray(image.data(), size))
                   .compare(expected, Qt::CaseInsensitive) != 0)
            fail(QStringLiteral("端口 %1 由其他程序占用。请确认启动的是所选整合包副本。").arg(port));
        return pid;
    };
    const auto pid = confirmOwner();
    const EngineInterfaceSnapshot snapshot{fetchApi(port, "config"), fetchApi(port, "info")};
    if (confirmOwner() != pid || !snapshot.config.value("dependencies").isArray()
        || snapshot.config.value("version").toString().isEmpty() || !snapshot.info.value("named_endpoints").isObject())
        fail(QStringLiteral("端口 %1 的服务已变化，或无法读取完整的接口快照。").arg(port));
    return snapshot;
}

QStringList compareEngineInterface(const EngineInterfaceSnapshot &observed, const QJsonObject &expected)
{
    QMap<int, QJsonObject> components;
    for (const auto &item : observed.config.value("components").toArray()) {
        const auto component = item.toObject();
        components.insert(component.value("id").toInt(-1), component);
    }
    const auto dependencies = observed.config.value("dependencies").toArray();
    const auto endpoints = observed.info.value("named_endpoints").toObject();
    QStringList differences;
    for (auto method = expected.constBegin(); method != expected.constEnd(); ++method) {
        const auto issue = interfaceChange(method.key(), method.value().toObject(), dependencies, components, endpoints);
        if (!issue.isEmpty())
            differences.append(issue);
    }
    return differences;
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

EnginePorts enginePorts(const QString &directory, const QString &adapter)
{
    const auto contract = engineContract(adapter);
    EnginePorts ports{quint16(contract.value("main_port").toInt(9874)),
        quint16(contract.value("inference_port").toInt(9872))};
    QFile file(checkedPath(directory + "/config.py"));
    if (file.open(QIODevice::ReadOnly)) {
        const auto configuration = QString::fromUtf8(file.read(1024 * 1024));
        const auto read = [&](const QString &name, quint16 fallback) {
            const QRegularExpression literal("^" + name + R"(\s*=\s*(\d{1,5})[ \t]*(?:#[^\r\n]*)?\r?$)",
                QRegularExpression::MultilineOption);
            const auto match = literal.match(configuration);
            const auto value = match.captured(1).toInt();
            if (!match.hasMatch())
                return fallback;
            if (value < 1 || value > 65535)
                fail(QStringLiteral("引擎配置中的端口无效：%1").arg(name));
            return quint16(value);
        };
        ports.main = read("webui_port_main", ports.main);
        ports.inference = read("webui_port_infer_tts", ports.inference);
    }
    if (ports.main == ports.inference)
        fail(QStringLiteral("主服务和推理服务需要分别配置端口。"));
    return ports;
}

QString engineFingerprint(const QString &directory)
{
    QCryptographicHash fingerprint(QCryptographicHash::Sha256);
    const QStringList files{"go-webui.bat", "webui.py", "config.py", "GPT_SoVITS/inference_webui_fast.py",
        "GPT_SoVITS/s2_train_v3_lora.py", "GPT_SoVITS/s1_train.py"};
    for (const auto &name : files) {
        QFile file(checkedPath(directory + '/' + name));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        fingerprint.addData(file.open(QIODevice::ReadOnly) && hash.addData(&file) ? hash.result() : QByteArray(32, '\0'));
    }
    const QDir packages(checkedPath(directory + "/runtime/Lib/site-packages"));
    for (const auto &entry : packages.entryList({"gradio-*.dist-info", "gradio_client-*.dist-info"}, QDir::Dirs, QDir::Name))
        fingerprint.addData(entry.toUtf8());
    return QString::fromLatin1(fingerprint.result().toHex());
}

QString resumeEngineIssue(const QString &directory, const QString &family, const QString &operation, const QString &adapter)
{
    if (family != "v5turbo")
        return QStringLiteral("当前续训适配限定 v5turbo。");
    const auto pinned = engineContract(adapter).value("resume").toObject();
    for (auto item = pinned.constBegin(); item != pinned.constEnd(); ++item) {
        if ((operation == "train_sovits" && item.key() == "GPT_SoVITS/s1_train.py")
            || (operation == "train_gpt" && item.key() == "GPT_SoVITS/s2_train_v3_lora.py"))
            continue;
        if (engineScriptDigest(directory, item.key()) != item.value().toString())
            return QStringLiteral("续训脚本缺失或摘要变化 %1。可重新开始该训练阶段。").arg(item.key());
    }
    return {};
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
    connect(&localDeviceProbe_, &QFutureWatcherBase::finished, this, [this] {
        localDevices_ = localDeviceProbe_.result();
        emit trainingDevicesChanged();
    });
    timer_.setInterval(750);
    connect(&timer_, &QTimer::timeout, this, &EngineManager::poll);
    connect(&probe_, &QFutureWatcher<EngineProbe>::finished, this, [this] {
        if (!process_ || ready_)
            return;
        const auto result = probe_.result();
        if (!result.responded)
            return;
        if (!result.error.isEmpty() || !ownsListener(ports_.main)) {
            failInstance(result.error.isEmpty() ? QStringLiteral("引擎监听进程已变化") : result.error);
            return;
        }
        unavailable_ = result.unavailable;
        devices_ = result.devices;
        gradio_ = result.gradio;
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
    localDeviceProbe_.waitForFinished();
}

void EngineManager::start(const QString &launcher, const QString &logPath, const QString &adapter)
{
    if (process_ || probe_.isRunning())
        fail(QStringLiteral("已有引擎正在启动或运行"));
    const auto dir = engineRoot(launcher);
    const auto issue = resourceIssue(dir, "start", {});
    if (!issue.isEmpty())
        fail(issue);
    const auto ports = enginePorts(dir, adapter);
    const auto identity = engineFingerprint(dir);
    for (auto port : {ports.main, ports.inference}) {
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
    adapter_ = adapter.isEmpty() ? defaultEngineAdapter() : adapter;
    fingerprint_ = identity;
    ports_ = ports;
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
    gradio_.clear();
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
    if (ownsListener(ports_.main))
        probe_.setFuture(QtConcurrent::run(probeApi, ports_.main, engineContract(adapter_)));
}

void EngineManager::failInstance(const QString &message)
{
    emit instanceFailed(root_ + "/go-webui.bat", message);
    stop();
    emit statusChanged(message);
}

bool EngineManager::ready() const { return ready_; }
bool EngineManager::active() const { return bool(process_); }
bool EngineManager::canStart() const { return !process_ && !probe_.isRunning(); }
QString EngineManager::root() const { return root_; }

QMap<QString, QString> EngineManager::interfaceIssues() const
{
    return unavailable_;
}

QString EngineManager::devices() const { return ready_ ? devices_ : QString(); }

QMap<int, QString> reportedTrainingDevices(const QString &report)
{
    static const QRegularExpression row(R"(^\s*(\d{1,2})\s+(.+)$)");
    QMap<int, QString> result;
    for (const auto &line : report.split('\n')) {
        const auto match = row.match(line.trimmed());
        if (!match.hasMatch())
            continue;
        const auto label = match.captured(2).simplified();
        result.insert(match.captured(1).toInt(), label.startsWith("CPU ") ? QStringLiteral("CPU") : label);
    }
    return result;
}

QMap<int, QString> EngineManager::trainingDevices(const QString &launcher) const
{
    if (ready_ && (root_ + "/go-webui.bat").compare(QDir::fromNativeSeparators(launcher), Qt::CaseInsensitive) == 0) {
        const auto reported = reportedTrainingDevices(devices_);
        if (!reported.isEmpty())
            return reported;
    }
    return localDevices_;
}

void EngineManager::discoverLocalDevices()
{
    localDeviceProbe_.setFuture(QtConcurrent::run(localCudaDevices));
}

QString EngineManager::gradioVersion() const { return ready_ ? gradio_ : QString(); }
EnginePorts EngineManager::ports() const { return ports_; }

quint32 EngineManager::webuiPid() const
{
    const auto pid = listener(ports_.main);
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
