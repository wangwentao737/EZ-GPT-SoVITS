# 单个 GPT-SoVITS 版本适配操作清单

一次处理一个整合包构建、一个模型家族和一项操作。先复制 [验收报告模板](registration-template.md)，再按下面的顺序操作。可以直接参照 [0725-cu124 实例](acceptance-0725-cu124.md) 和 [清单中的两条整合包记录](registry-v1.json)。

## 0. 从上游主分支开始

- [ ] 确认本次适配的包标识和范围。下面用 `engine-new` 作包 ID 示例，实际操作时替换为你的包 ID。
- [ ] 查看远程地址、当前分支和暂存区。有上游仓库写权限时，可以向上游推送功能分支。使用个人 fork 时，先将原仓库和 fork 对应到各自的远程名。
- [ ] 确认上游目标分支已有兼容清单机制和要复用的适配器。若相关代码仍在本地，先合入基础功能，或将依赖代码随适配器 PR 一起评审。

```powershell
git remote -v
git status --short --branch
git diff --cached --name-only

$repo = 'wangwentao737/EZ-GPT-SoVITS'
$baseRemote = 'origin'  # origin 指向个人 fork 时，填指向原仓库的远程名，例如 upstream
$baseRef = "$baseRemote/main"
$packageId = 'engine-new'
$branch = "compat/$packageId"
git fetch $baseRemote
if ($LASTEXITCODE -ne 0) { throw '获取上游主分支失败' }
```

工作区干净时执行 `git switch -c $branch $baseRef`。当前已有其他改动或暂存文件时，从上游创建独立工作树，在那里做本次适配，或按文件带入已经完成的本次改动。

```powershell
$worktree = Join-Path (Split-Path -Parent $PWD.Path) "EZ-GPT-SoVITS-$packageId"
git worktree add -b $branch $worktree $baseRef
if ($LASTEXITCODE -ne 0) { throw '创建适配工作树失败' }
Set-Location $worktree
```

## 1. 准备独立样本

- [ ] 把目标整合包复制到仓库下的 `.test/engine-new`。保留原包，启动和测试都在副本上进行。
- [ ] 记录包的下载来源或归档标识、Windows 和 GPU、EZ 版本或提交号。有上游版本声明时一并记录出处。
- [ ] 准备一对同家族的 GPT `.ckpt` 和 SoVITS `.pth`、3 至 10 秒的清晰 WAV、参考文本和短台词。每个要登记的家族都准备一对配套权重。
- [ ] 启动副本的主服务。采集合成接口时，再从主服务打开快速推理。先确认两个端口都属于这个副本。

从仓库根目录运行以下命令，每次使用一个尚未创建的输出目录。示例使用整合包的默认端口。只采集主服务时，省略 `--inference-port 9872`。

```powershell
python tests/collect-engine-compatibility.py --engine .test/engine-new --output .test/capture-new --main-port 9874 --inference-port 9872 --source "填写归档标识"
```

采集结果包含 `capture.json`、主服务和推理服务的 `config`、`info` 快照。身份匹配使用 `webui.py` 和 `GPT_SoVITS/inference_webui_fast.py` 的 SHA-256。BAT 摘要可以附上供检查。目录名和 Gradio 版本用于显示和诊断。

## 2. 决定改清单还是改代码

打开 `engine/` 下已有的 `*-contract.json`。对照采集的 `config` 和 `info`，逐项核对主服务操作使用的事件、快速推理事件、入参数量和顺序、名字、控件标签、类型、返回值和关闭接口。还要检查模型目录、BERT、HuBERT，以及对应家族需要的额外模型。相同数量的参数也要按顺序核对。

| 采集结果 | 怎么做 |
|---|---|
| 接口、权重布局和本机资源规则都匹配已有适配器 | 复用该适配器。完成实测后更新清单，现有 EZ 构建即可读取。 |
| 只有目录名、BAT 参数或服务端口变了 | 核对主服务及推理服务是否启动在所选副本。可以继续使用已有适配器。 |
| 参数、返回结构、模型路径或运行方式有变化 | 先修改相应契约或适配逻辑，构建测试版，再实测和登记。 |

0725-cu124 就是第三种情况。它与 20261006 的四个主服务合成事件兼容，快速推理的 21 项参数需要不同的映射。

### 需要新适配器时

- [ ] 在 `engine/` 新建 `<适配器>-contract.json`。从采集记录填写 `id`、两个端口、实际调用的主服务事件、对应操作和推理事件。按顺序填写每项入参和返回值。
- [ ] 在 `src/compatibility.cpp` 登记契约文件名、已实现的操作和模型家族。若文件布局变化，在 `src/resources.cpp` 增加对应资源规则并保留旧规则，同步接入清单校验、界面筛选、任务预检和桥接器的路径。
- [ ] 在 `engine/bridge.py` 登记适配器及参数映射。检查参考音频、随机种子、模型暂存、推理关闭和 WAV 返回值。老适配器的参数映射也要跑回归。
- [ ] 在 `CMakeLists.txt` 的 `EZ_ENGINE_CONTRACTS` 中加入契约，运行 [README 的构建命令](../README.md#构建)。启用 `-DEZ_BUILD_CHECKS=ON` 可同时构建组件检查。

## 3. 在本地形成候选清单

- [ ] 从 `compat/registry-v1.json` 复制一份到 `.test/registry-candidate.json`。递增 `revision`，核对适用的 `ez_min` 和 `ez_max`。
- [ ] 在 `packages` 新增唯一的包 `id`，填写 `name`、`identity_source`、`adapter` 和两个身份脚本摘要。有可信上游声明时可填写 `upstream_version`。保留要回归的既有包条目。
- [ ] 在该包的 `capabilities` 中逐项填写 `operation`、`resources`、`result` 和 `report`。合成和训练类填写 `family`，自动转写填写 `language`。复用本项目现有资源规则时，`resources` 为 `20261006`。失败项可附 `reason`，计划补测的组合省略或使用 `unverified`。
- [ ] 用测试数据目录启动 EZ，在「设置」中导入候选清单，再添加副本里的 `go-webui.bat`。核对「引擎详情」显示的包、适配器和清单修订。

实测时可在隔离数据目录里临时开放正在验证的组合。实测完成后，只有具备通过证据的组合可以标为 `passed`。候选清单和测试音频都留在测试工作区。

## 4. 从 EZ 实测

- [ ] 在「语音合成」确认只出现已登记且本机资源齐全的家族。导入每个家族的权重和参考音频，用同一句短台词分别合成。试听归档 WAV，记录任务号、非静音结果、时长、种子和 SHA-256。
- [ ] 每个要开放的训练环节都从 EZ 单独测试。准备数据、SoVITS 和 GPT 各记一次，核对两份新权重并试音。要开放续训，再分别中断、恢复并核对实际恢复轮次。普通话和粤语转写分别登记。
- [ ] 验证拒绝路径。移走某一项必要资源时详情应指出相对路径。接口改变时应在发送对应业务调用前拦截。恢复文件后重新检测。
- [ ] 切回 20261006，在同一个 EZ 构建中确认旧声音、任务和可用家族都能正常使用。核对至少一条归档 WAV。重大适配改动再跑对应家族及训练回归。

可用命令行重复合成验收。把请求保存为 JSON 数组，字段使用下方示例。把权重和参考音频复制到本次测试数据目录下，并填入其实际绝对路径。

```json
[
  {
    "operation": "synthesize",
    "family": "v2",
    "gpt": "E:/test-data/sample.ckpt",
    "sovits": "E:/test-data/sample.pth",
    "reference": "E:/test-data/reference.wav",
    "prompt": "参考录音里实际说的话。",
    "prompt_language": "中文",
    "text": "这是一句用于验收的台词。",
    "language": "中文",
    "split": "不切",
    "seed": 42
  }
]
```

先在测试数据目录导入候选清单并退出 EZ，再用同一数据目录运行队列。把示例中的 EXE、数据目录和请求文件替换为实际路径。结果会写入该目录的 `library.sqlite`，WAV 归档在 `outputs` 下。

```powershell
$ez = (Resolve-Path out/portable/EZ-GPT-SoVITS.exe).Path
$data = (Resolve-Path .test/compat-data).Path
$bat = (Resolve-Path .test/engine-new/go-webui.bat).Path
$requests = (Resolve-Path .test/requests.json).Path
$argsForEz = "--data-root `"$data`" --probe-engine `"$bat`" --probe-queue --probe-job `"$requests`""
$run = Start-Process -FilePath $ez -ArgumentList $argsForEz -WindowStyle Hidden -Wait -PassThru
$run.ExitCode
```

退出码为 `0` 后，再打开同一数据目录核对任务记录和 WAV。遇到失败则展开任务详情，查看对应任务日志和 `engine-start.log`。维护 0725 或 20261006 时，还可用 `tests/engine-compatibility.py --help` 查看现成的模型家族回归入口。

## 5. 整理提交

- [ ] 将实测结果写到 `compat/acceptance-<包ID>.md`，保留包来源、脚本摘要、EZ 构建、操作和家族、任务号、输出摘要与局限。每条通过记录的 `report` 都应指向报告中确实存在的章节。
- [ ] 把已验收的条目写入 `compat/registry-v1.json`。用另一个测试数据目录检查离线导入、重复导入和失败回退。提 PR 前再次核对上游 `main` 中的 `revision`，正式候选清单的修订号应高于它。首次上线时，以待部署构建的内置清单修订为基线核对。
- [ ] 只暂存本次提交所需的文件。模型权重、录音、WAV、SQLite、完整接口快照、运行日志和便携构建放在 `.test` 或 `out`。进入 Git 的接口样本只保留用于复现差异的字段，并检查是否含本机路径、凭据或私有内容。

| PR 类型 | 应提交的内容 | 验收依据 |
|---|---|---|
| 复用已有适配器 | `compat/registry-v1.json` 和对应验收报告 | 用目标 EZ 构建导入清单，核对新包、20261006 和至少一条拒绝路径 |
| 新增或修改适配器 | 上述文件及实际改动的契约、桥接、资源规则、打包配置、回归测试和最小接口样本 | 用新构建实测受影响家族，记录通过和待验收项，再跑既有适配器回归 |

下面是清单与报告的暂存示例。代码型 PR 再按实际改动逐个加入文件，比如 `engine/<适配器>-contract.json`、`engine/bridge.py`、`src/compatibility.cpp`、`CMakeLists.txt` 和相应测试。

```powershell
$report = "compat/acceptance-$packageId.md"
python -m json.tool compat/registry-v1.json | Out-Null
if ($LASTEXITCODE -ne 0) { throw '兼容清单 JSON 无效' }

git add -- compat/registry-v1.json $report
git diff --cached --check
git diff --cached --name-status
git diff --cached
```

确认暂存区只包含当前 PR 的内容后提交。把构建命令、测试脚本与人工验收结果一起记到 PR 正文里。

```powershell
git commit -m "compat: register $packageId"
git log --oneline "${baseRef}..HEAD"
git diff --stat "${baseRef}...HEAD"
```

## 6. 推送分支并创建 PR

下面使用 GitHub CLI。先用 `gh auth status` 查看账号，需要登录时运行 `gh auth login`。两种推送方式择一，目标 PR 的 base 都是原仓库的 `main`。

**可以向原仓库推送功能分支时**

```powershell
$pushRemote = $baseRemote
$head = $branch
```

**向个人 fork 推送时**

还没有 fork 时运行 `gh repo fork $repo --clone=false --remote=false`。下面用 `fork` 作个人 fork 的远程名。如果它已存在，先用 `git remote get-url fork` 核对地址，再复用该远程。

```powershell
$githubUser = gh api user --jq .login
if ($LASTEXITCODE -ne 0) { throw '无法读取当前 GitHub 账号' }
if ('fork' -notin (git remote)) {
    git remote add fork "https://github.com/$githubUser/EZ-GPT-SoVITS.git"
    if ($LASTEXITCODE -ne 0) { throw '添加个人 fork 远程失败' }
}
git remote get-url fork
$pushRemote = 'fork'
$head = "${githubUser}:$branch"
```

推送当前功能分支。然后复制下面的 PR 正文，逐条替换括号里的内容，把已验收和待验收的操作都写清楚。若有关联 issue，在正文中附上编号。

```powershell
git push -u $pushRemote $branch
if ($LASTEXITCODE -ne 0) { throw '推送功能分支失败' }

New-Item -ItemType Directory -Path .test -Force | Out-Null
$prBody = Join-Path $PWD.Path '.test/compat-pr.md'
@'
## 适配范围

- 包 ID、归档来源、两个身份脚本的摘要
- 复用的适配器或本次修改的协议、资源规则
- 清单修订号、适用的 EZ 版本和已验证的操作、模型家族

## 验收证据

- 验收报告路径、测试构建号、Windows 和 GPU
- 目标包的测试步骤或命令、任务号、产物时长和 SHA-256
- 20261006 回归结果、切换整合包和拒绝路径的结果
- 原生 WebUI 与 EZ 的结果分开记录

## 发布影响

- 只需更新清单，或需要搭配新 EZ 构建
- 后续待验收的家族和操作
- 如需撤回，应在更高修订中移除或改写的能力项
'@ | Set-Content -LiteralPath $prBody -Encoding utf8
```

编辑并核对 `$prBody` 后创建 PR。验收尚在进行时，可在创建命令末尾加 `--draft`。

```powershell
gh pr create --repo $repo --base main --head $head --title "compat: support $packageId" --body-file $prBody
```

也可以在 GitHub 网页上进入原仓库的 Pull requests，选择 `New pull request`。将 base 设为原仓库的 `main`，compare 设为已推送的功能分支。来自 fork 的分支通过 `compare across forks` 选择。提交前检查页面显示的文件、来源分支和目标分支。[创建 PR 的 CLI 参数](https://cli.github.com/manual/gh_pr_create) · [从 fork 提交 PR](https://docs.github.com/en/pull-requests/how-tos/create-pull-requests/creating-a-pull-request-from-a-fork)

## 7. 评审、合并和验证

- [ ] 在 PR 的 Files changed 中复查只包含本次修改。用下列命令查看 PR 和 CI。真实模型实测仍以报告为准。仓库没有运行对应检查时，把本地结果写明。
- [ ] 按评审意见在同一功能分支补提交，再推送同一分支，原 PR 会更新。修订号冲突时，先同步最新上游 `main`，人工保留其他已登记条目，再选一个更高的修订号。解决冲突后重新校验 JSON 并跑相应回归。
- [ ] 草稿验收完成后转为正式 PR。通过仓库要求的检查和评审后，由有权限的维护者在 GitHub 上合并。代码型 PR 在可用的新构建上验收，发布说明注明最低可用构建或版本。

```powershell
$pr = 123  # 替换为实际 PR 编号
gh pr view $pr --repo $repo --web
gh pr diff $pr --repo $repo
gh pr checks $pr --repo $repo --watch
# 如为草稿且已经完成验收
gh pr ready $pr --repo $repo
```

评审要求更新到最新上游时，在同一功能分支执行 `git fetch $baseRemote` 和 `git merge $baseRef`。有冲突时用 `git status` 找到文件，合并完登记条目和修订号后，逐个暂存解决的文件，用 `git merge --continue` 完成合并。完成回归后再次执行 `git push $pushRemote $branch`。

合并后，先检查上游 `main` 实际提供的清单，再用目标 EZ 构建进入「设置 → 整合包兼容清单 → 检查清单更新」。核对修订号、目标包和 20261006 的选项，重复检查一次应提示已是最新。再用一个独立数据目录做离线导入。新适配器随支持它的 EZ 构建交付。遇到旧构建拒绝未知适配器时，旧构建会保留上一次可用清单。

```powershell
$url = "https://api.github.com/repos/$repo/contents/compat/registry-v1.json?ref=main"
$response = Invoke-WebRequest -Uri $url -Headers @{
    Accept = 'application/vnd.github.raw+json'
    'User-Agent' = 'EZ-GPT-SoVITS'
}
$onlineRegistry = ConvertFrom-Json -InputObject $response.Content
$onlineRegistry.revision
$onlineRegistry.packages | Select-Object id, adapter
```

如果读取失败或在线修订与预期有差异，先核对合并目标、文件路径及线上内容。把实测结果、合并提交和已验证的在线修订记回 PR。[查看 PR 检查](https://cli.github.com/manual/gh_pr_checks) · [读取 GitHub 文件的接口](https://docs.github.com/en/rest/repos/contents#get-repository-content)

## 8. 撤回已发布登记

- [ ] 从最新的上游 `main` 新建修复分支。保留其他已上线整合包，删除有误的能力项，或改为 `failed` 并写明原因。
- [ ] 将修订号提高到当前线上修订之后，用测试工作区导入验证，确认受影响功能已撤下，其他已登记包仍可用。
- [ ] 按第 5 至 7 节提交并验证撤回 PR。只维护本机时，直接导入修订号更高的撤回清单。客户端只接受当前修订的相同内容或更高修订。
