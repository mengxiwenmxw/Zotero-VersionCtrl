# 架构概览（草案）

目标是实现一个单二进制跨平台应用，主要模块：

- `device`：设备发现与注册（tailscale peers、活动检查）
- `mount`：远程共享访问抽象（SMB on Linux、UNC on Windows）
- `sync`：文件比较、增量复制与事务性写入
- `vcs`：轻量变更树、commit 与回滚
- `cli`：命令行与交互式 shell（REPL）

数据目录：`.zvcs/` 存放对象、索引与元数据。
