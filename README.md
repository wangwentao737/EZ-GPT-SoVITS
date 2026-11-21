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

从便携目录打开 `EZ-GPT-SoVITS.exe` 即进入工作台。新安装的素材和项目保存在 EXE 同级的 `data`，默认成品目录为 `data/outputs`。「设置」页可添加引擎启动路径及更改成品生成目录。开发联调可以通过 `--data-root` 指定独立的数据目录。

运行 `pwsh -File tests/portable-smoke.ps1` 验证便携启动、SQLite 初始化、端口冲突与目录联接拒绝。运行时需让 9874 端口空闲，以便脚本创建临时监听器。

如开发机安装了 Python，也可运行 `python -B tests/task-recovery.py`。该脚本通过便携 EXE 验证归档提交和训练阶段的重启恢复，测试数据写入项目的 `.test` 目录。

如已在 `.test` 中备好独立引擎和引擎联调样例，可运行 `python -X utf8 -B tests/live-cancellation.py` 验证运行中取消和后续任务续跑。脚本的 `--help` 列出样例路径选项。

同一套夹具可用 `python -X utf8 -B tests/resource-preflight.py` 验证缺少启动资源、接口变化和损坏权重的处理，以及失败后的真实合成。

`python -X utf8 -B tests/training-integrity.py` 使用独立引擎实测预处理、样本错配拦截、两阶段训练和新权重试听。训练中断后可以从最后成功的阶段继续，重跑该阶段会建立独立检查点目录。

## 开发接入

- 在「设置」手动添加已安装的 GPT-SoVITS 中的 `go-webui.bat`，再在「工作台」检查模型家族并启动引擎。引擎会在所选安装目录中写入运行文件。EZ 使用本机 Gradio 接口。目标整合包是 GPT-SoVITS-v5-20261006，主服务使用 9874，快速推理服务使用 9872。
- 「工作台」可检查引擎资源、指定声音及磁盘条件。修复后，在失败项下拉列表中选择一项并复查。「导出诊断」可预览和复制报告，原始日志由用户勾选。要导出某条历史任务，到「任务记录 → 更多操作 → 导出所选任务诊断」。
- 在「语音合成」导入配套的 GPT 和 SoVITS 权重、参考音频和参考文本。选择声音并输入台词，生成后的 WAV 可试听或另存。
- 在「声音与素材」按类型、项目或声音筛选。选择一项可查看文件位置和关联任务，也可以重命名声音预设。未被引用的素材库副本可以移至回收站。
- 在「训练项目」创建项目、导入录音、切片及校对。保存标注并准备数据后，启动 SoVITS 和 GPT 训练。选中片段可用作训练后试听的参考音频。
- 「任务记录」保存任务状态、参数与日志路径。取消时会先请求停止当前任务，超时后关闭本程序启动的引擎进程。队列里仍有任务时会尝试重启引擎并继续执行。失败的合成可重试，训练任务从项目内重新提交。

引擎副本、测试样本、Qt 开发依赖及发行产物均由 Git 忽略。开发联调使用独立的引擎副本，原始安装只用于只读核对。
