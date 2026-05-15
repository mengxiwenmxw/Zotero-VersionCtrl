# zvcs

`zvcs` 是一个面向 Zotero 文献库的轻量同步与版本管理工具原型，目标是把常见的“多设备文献库同步 + 本地历史快照 + 回滚”串成一个可执行的单二进制工具。

当前仓库已经可以初步使用，主要能力包括：

- 基于 `peers.conf` 的 peer 路径检查
- 从 peer 路径拉取缺失或变化的文件
- 记录本地快照、提交信息和提交历史
- 按提交前缀回滚文件
- 面向 Zotero 常见文件类型的测试覆盖

## 现状说明

这个项目还不是完整的 Git 替代品，但已经形成了一个可用的框架：

- `init` 初始化本地 `.zvcs` 目录
- `status` 查看工作区和仓库状态
- `check` 检查 `peers.conf` 中的路径是否可用
- `fetch` 只同步缺失或内容变化的文件
- `commit` 生成本地快照
- `log` 查看提交历史
- `checkout` 按提交前缀恢复文件

如果你准备把它交给其他人试用，这份 README 里描述的就是当前真实行为。

## 构建

```sh
make
```

构建产物默认在 `build/zvcs`。

Windows 交叉编译：

```sh
make windows
```

产物为 `build/zvcs.exe`，需要本机已安装 `x86_64-w64-mingw32-gcc`。

## 快速开始

```sh
./build/zvcs help
./build/zvcs init
./build/zvcs status
```

建议先在 Zotero 文献库根目录执行一次 `init`，然后配置 peer 路径并运行 `check` / `fetch`。

## 配置方式

在可执行文件同级目录放置 `peers.conf`，每行一个 peer 文献库路径：

- Linux：已挂载的目录路径
- Windows：盘符路径、UNC 路径或网络映射路径

示例：

```conf
Z:\Papers\Zotero
/mnt/zotero/peer-a
```

`check` 会逐个验证路径是否存在、是否为目录、是否可打开。

`fetch` 会读取这些路径，把变化的文件同步到当前工作目录。

## 工作目录与数据目录

`zvcs` 在当前 Zotero 库目录下工作，数据保存在：

- `.zvcs/objects`：文件内容对象
- `.zvcs/commits`：提交记录
- `.zvcs/index`：当前索引

同时会忽略以下内容：

- `.zvcs/`
- `.zvcsignore`
- `peers.conf`
- `zvcs` / `zvcs.exe`

## 命令说明

### `help`
显示帮助信息。

### `init`
初始化本地 `.zvcs` 仓库结构。

### `status`
显示：

- 仓库目录是否存在
- 已跟踪文件的修改/删除情况
- 未跟踪文件

### `check`
检查 `peers.conf` 中列出的 peer 路径。

### `fetch`
从 peer 拉取文件到当前工作目录。

当前行为：

- 只复制缺失或内容变化的文件
- 对 Zotero 常见数据库文件按组处理
- SQLite 主库与 sidecar 采用临时文件落地后再替换
- 输出复制汇总和失败文件列表

### `commit [-m message]`
创建一次本地快照。

如果不带 `-m`，会打开默认编辑器填写提交说明。

也支持直接写成：

```sh
zvcs commit -m "message"
```

### `log`
查看本地提交历史，最新提交在前。

### `checkout <commit-prefix>`
根据提交前缀恢复文件到指定版本。

注意：

- 这是直接恢复工作区文件的操作
- 不会先做类似 Git 的工作区保护确认

## 测试

推荐直接跑集成脚本：

```sh
bash test/test_databank_sync.sh
```

这个脚本会使用：

- `test/databankA`
- `test/databankB`

作为两个真实文献库 fixture，复制到临时目录后执行：

- `commit`
- `log`
- `status`
- `checkout`
- `check`
- `fetch`

脚本里还覆盖了：

- PDF
- PNG
- SQLite 数据库
- 嵌套目录
- 交互式提交说明

## 已知限制

- 目前 peer 发现仍然依赖 `peers.conf`，还没有自动发现网络设备
- `.zvcsignore` 只支持简单路径前缀
- `checkout` 不会像 Git 那样在有冲突时做复杂保护
- 这仍然是原型级框架，但已可作为初步可用版本进行试用

## 维护说明

仓库内还有一份架构说明，描述当前模块划分和演进方向：

- [docs/architecture.md](/mnt/e/Projects/Zotero-VersionCtrl/docs/architecture.md)

测试入口在：

- [test/test_databank_sync.sh](/mnt/e/Projects/Zotero-VersionCtrl/test/test_databank_sync.sh)
