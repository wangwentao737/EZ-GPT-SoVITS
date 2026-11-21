# EZ-GPT-SoVITS

EZ-GPT-SoVITS 是使用本地 GPT-SoVITS 整合包的中文 Windows 语音工作台。开发规格见 [开发文档](开发文档.md)。

## 构建

需要 Windows x64、Visual Studio 2022 的 C++ 桌面开发工作负载、CMake 3.24 或更新版本，以及 Qt 6.8.3 MSVC 2022 x64。Qt 安装需包含 Qt Multimedia。

在项目目录运行以下命令，将 Qt 路径替换为实际安装位置。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 "-DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64"
cmake --build build --config Release --parallel
cmake --install build --config Release --prefix out/portable
```

从便携目录打开 `EZ-GPT-SoVITS.exe`。默认数据目录为 `%LOCALAPPDATA%\\EZ-GPT-SoVITS`，开发联调可以通过 `--data-root` 指定绝对路径。

## 开发接入

- 在「工作台」手动添加独立整合包的 `go-webui.bat`，选择要检查的模型家族，然后启动引擎。EZ 使用本机 Gradio 接口。目标整合包是 GPT-SoVITS-v5-20261006，主服务使用 9874，快速推理服务使用 9872。
- 在「语音合成」导入配套的 GPT 和 SoVITS 权重、参考音频和参考文本。选择声音并输入台词，生成后的 WAV 可试听或另存。
- 在「训练项目」创建项目、导入录音、切片及校对。保存标注并准备数据后，启动 SoVITS 和 GPT 训练。选中片段可用作训练后试听的参考音频。
- 「任务记录」保存任务状态、参数与日志路径。可取消当前任务，重试失败的合成。训练任务从项目内重新提交。

引擎副本、测试样本、Qt 开发依赖及发行产物均由 Git 忽略。现有受保护安装用于只读核对。引擎运行、测试和训练使用独立副本。
