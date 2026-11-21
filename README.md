# EZ-GPT-SoVITS

EZ-GPT-SoVITS 是使用本地 GPT-SoVITS 整合包的中文 Windows 语音工作台。开发规格见 [开发文档](开发文档.md)。

## 构建

需要 Windows x64、Visual Studio 2022 的 C++ 桌面开发工作负载、CMake 3.24 或更新版本，以及 Qt 6.8.3 MSVC 2022 x64。Qt 安装需包含 Qt Multimedia。

在项目目录运行以下命令，将 Qt 路径替换为实际安装位置。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 "-DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64"
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix (Join-Path $PWD "out/portable")
```

从便携目录打开 `EZ-GPT-SoVITS.exe`。默认数据目录为 `%LOCALAPPDATA%\\EZ-GPT-SoVITS`，开发联调可以通过 `--data-root` 指定绝对路径。

运行 `pwsh -File tests/portable-smoke.ps1` 验证便携启动、SQLite 初始化、端口冲突与目录联接拒绝。运行时需让 9874 端口空闲，以便脚本创建临时监听器。

如开发机安装了 Python，也可运行 `python -B tests/task-recovery.py`。该脚本通过便携 EXE 验证归档提交和训练阶段的重启恢复，测试数据写入项目的 `.test` 目录。

如已在 `.test` 中备好独立引擎和引擎联调样例，可运行 `python -X utf8 -B tests/live-cancellation.py` 验证运行中取消和后续任务续跑。脚本的 `--help` 列出样例路径选项。

## 开发接入

- 在「工作台」手动添加独立整合包的 `go-webui.bat`，选择要检查的模型家族，然后启动引擎。EZ 使用本机 Gradio 接口。目标整合包是 GPT-SoVITS-v5-20261006，主服务使用 9874，快速推理服务使用 9872。
- 在「语音合成」导入配套的 GPT 和 SoVITS 权重、参考音频和参考文本。选择声音并输入台词，生成后的 WAV 可试听或另存。
- 在「训练项目」创建项目、导入录音、切片及校对。保存标注并准备数据后，启动 SoVITS 和 GPT 训练。选中片段可用作训练后试听的参考音频。
- 「任务记录」保存任务状态、参数与日志路径。取消时会先请求停止当前任务，超时后关闭本程序启动的引擎进程。队列里仍有任务时会尝试重启引擎并继续执行。失败的合成可重试，训练任务从项目内重新提交。

引擎副本、测试样本、Qt 开发依赖及发行产物均由 Git 忽略。现有受保护安装用于只读核对。引擎运行、测试和训练使用独立副本。
