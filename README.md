# zvcs — Zotero Version Control Sync

这是一个用 C 语言实现的 Zotero 多设备同步工具原型。当前版本已经具备：

- 命令行入口
- peer 可用性检查
- 从 peer 路径拉取文件
- 本地 `.zvcs` 对象存储
- commit / log / checkout

## 快速开始

```sh
make
./build/zvcs help
```

## 配置

在可执行文件同级目录创建 `peers.conf`，每行写一个远程 Zotero 库路径：

- Linux 挂载后的目录路径
- Windows UNC 或映射盘路径

`./build/zvcs check` 会验证这些路径是否可访问。
`./build/zvcs fetch` 会从这些路径同步缺失或不同的文件到当前工作目录。

## VCS 数据

`.zvcs/` 目录用于保存：

- `objects/` 文件 blob
- `commits/` commit 记录
- `index` 最近一次提交的索引

## 命令

- `help`
- `init`
- `status`
- `check`
- `fetch`
- `commit [message]`
- `log`
- `checkout <commit-prefix>`

## 当前限制

- peer 发现仍然是配置驱动，不会自动调用 tailscale API。
- `checkout` 会直接恢复文件，不会先做工作区保护判断。
- `.zvcsignore` 只支持简单路径前缀。
