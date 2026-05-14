# zvcs — Zotero Version Control Sync (C prototype)

这是一个用 C 语言实现的 Zotero 多设备同步工具骨架项目。当前为初始骨架，包含 CLI、设备发现与同步的 stub。


快速开始：

```sh
make
./build/zvcs help
```

配置说明：在工具二进制文件同级目录创建 `peers.conf`，每行写一个该设备可见的远程 Zotero 库路径（绝对路径或已挂载的 UNC/SMB 路径）。运行 `./build/zvcs fetch` 时会读取该文件并从每个列出的路径将缺失或不同的文件同步到当前工作目录。

下步计划：实现 tailscale peers 发现、SMB/UNC 访问、文件比较与安全复制、轻量 VCS 对象存储与回滚。
