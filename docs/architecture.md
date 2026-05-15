# 架构说明

本文档描述当前 `zvcs` 的实际实现，而不是早期设想。它的目标是给后续维护者和使用者一个清晰的整体视图。

## 目标

`zvcs` 目前定位为：

- Zotero 文献库的多设备同步工具
- 带本地快照与回滚能力的轻量版本管理工具
- 适合在 Windows 和 Linux 下以单二进制方式部署

它不是完整的 Git，也不是 Zotero 的官方同步替代品。

## 当前模块

### `main`

命令行入口，负责：

- 解析命令
- 处理 commit 说明
- 打开默认编辑器
- 调度 `init` / `status` / `check` / `fetch` / `commit` / `log` / `checkout`

### `portable`

跨平台适配层，负责：

- 获取可执行文件目录
- 创建目录
- 拼接路径
- 原子替换文件
- 颜色输出能力探测

这里是 Windows 和 POSIX 差异的主要收口点。

### `fileutil`

文件与同步相关基础能力，负责：

- 计算 SHA-256
- 创建父目录
- 原子复制普通文件
- 生成同步计划
- 执行同步计划
- SQLite 主库和 sidecar 的临时文件复制与原子替换

### `sync`

peer 同步入口，负责：

- 读取 `peers.conf`
- 检查 peer 路径
- 从 peer 发起同步
- 输出同步摘要和错误信息

### `vcs`

本地历史管理，负责：

- 初始化 `.zvcs`
- 收集工作区文件
- 写入 objects / commits / index
- 读取 log
- 按提交恢复文件
- 查看 status

### `device`

当前实现是简化版，只输出本机与已配置 peers 的状态信息。

## 数据布局

工作目录下会生成：

- `.zvcs/objects`
- `.zvcs/commits`
- `.zvcs/index`

配置文件：

- `peers.conf`

忽略项：

- `.zvcs`
- `.zvcsignore`
- `peers.conf`
- `zvcs`
- `zvcs.exe`

## 同步流程

`fetch` 的实际流程是：

1. 读取 `peers.conf`
2. 遍历每个 peer 根目录
3. 生成同步计划
4. 对普通文件做“缺失或内容变化”判断
5. 对 SQLite 主库和 sidecar 做分组判断
6. 先写临时文件，再原子替换目标文件
7. 输出汇总与失败列表

这样做的原因是：

- 避免每次都整库重拷
- 降低 Zotero 数据库文件复制到一半导致损坏的风险
- 让 Windows 下的行为尽量接近 POSIX 原子更新

## 事务式 SQLite 复制

SQLite 文件在 Zotero 场景下通常不只是一个主库文件，可能还会有：

- `-wal`
- `-shm`
- `-journal`

当前实现会：

- 把主库和存在的 sidecar 分别复制到临时文件
- 所有临时文件成功后，再统一替换到目标位置

这比直接覆盖更稳，尤其适合数据库类文件。

## 测试框架

`test/test_databank_sync.sh` 是当前主要集成测试。

它使用：

- `test/databankA`
- `test/databankB`

作为两个文献库 fixture，验证：

- 目录层级文件
- PDF
- PNG
- SQLite
- 交互式 commit
- check / fetch / checkout

## 现阶段限制

- peer 发现仍然依赖配置文件
- `checkout` 不做复杂冲突保护
- `.zvcsignore` 规则较简单
- `device` 仍是占位式实现

## 后续扩展点

如果继续完善，比较自然的方向是：

- 更细粒度的同步统计
- 更强的冲突检测
- 更像 Git 的暂存区与差异展示
- 更完整的 Windows 交互体验

目前仓库已经具备“初步可用”的基础框架，可以作为后续迭代的起点。
