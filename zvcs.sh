#!/bin/bash
# zotero_vcs.sh - Zotero版本控制系统
# 用于在两台Windows电脑间通过Xubuntu同步Zotero数据并管理版本

set -euo pipefail

# ========== 配置部分 ==========
REPO_BASE=$repo_base         # 版本库根目录
LAPTOP_MOUNT=$laptop_zotero_mount         # laptop挂载点
PC_MOUNT=$pc_zotero_mount                 # pc挂载点
LOG_FILE="$REPO_BASE/zotero_vcs.log"      # 日志文件

# 确保目录存在
mkdir -p "$REPO_BASE"/{laptop/{snapshots,current,metadata},pc/{snapshots,current,metadata},merged}
touch "$LOG_FILE"

# ========== 工具函数 ==========
log() {
    local level=$1
    local message=$2
    local timestamp=$(date '+%Y-%m-%d %H:%M:%S')
    echo "[$timestamp] [$level] $message" | tee -a "$LOG_FILE"
}

check_mount() {
    local mount_point=$1
    local name=$2
    
    if ! mountpoint -q "$mount_point"; then
        log "ERROR" "$name 挂载点 '$mount_point' 未挂载或不可访问"
        return 1
    fi
    
    if [ ! -d "$mount_point" ]; then
        log "WARN" "$name 的Zotero数据目录不存在: $mount_point"
        return 2
    fi
    
    log "INFO" "$name 挂载点检查通过"
    return 0
}

generate_snapshot_id() {
    date '+%Y%m%d_%H%M%S'
}

# ========== 核心命令 ==========

# 初始化版本库
cmd_init() {
    log "INFO" "初始化Zotero版本控制系统"
    
    echo "正在检查挂载点..."
    check_mount "$LAPTOP_MOUNT" "Laptop" || return 1
    check_mount "$PC_MOUNT" "PC" || return 1
    
    # 创建初始快照
    cmd_commit "laptop" "初始化: laptop初始快照"
    cmd_commit "pc" "初始化: pc初始快照"
    
    # 创建合并状态
    sync_merged_state
    
    log "SUCCESS" "版本库初始化完成"
    echo "版本库位置: $REPO_BASE"
    echo "使用 './zotero_vcs.sh status' 查看状态"
}

# 创建快照
cmd_commit() {
    local source=$1  # "laptop" 或 "pc"
    local message=${2:-"自动提交"}
    local timestamp=$(generate_snapshot_id)
    local source_mount=""
    local snapshot_dir="$REPO_BASE/$source/snapshots/$timestamp"
    
    case $source in
        "laptop") source_mount="$LAPTOP_MOUNT" ;;
        "pc") source_mount="$PC_MOUNT" ;;
        *) log "ERROR" "未知数据源: $source"; return 1 ;;
    esac
    
    # 检查挂载点
    if ! check_mount "$source_mount" "$source"; then
        return 1
    fi
    
    log "INFO" "为 $source 创建快照: $timestamp"
    
    # 创建快照目录
    mkdir -p "$snapshot_dir"
    
    # 使用rsync创建硬链接快照（节省空间）
    local prev_snapshot=$(find "$REPO_BASE/$source/snapshots" -maxdepth 1 -type d -name "20*" | sort -r | head -2 | tail -1)
    
    if [ -n "$prev_snapshot" ] && [ -d "$prev_snapshot" ]; then
        # 使用硬链接到上一个快照，只复制变化的文件
        log "INFO" "使用增量快照 (基于: $(basename "$prev_snapshot"))"
        rsync -a --link-dest="$prev_snapshot" \
              "$source_mount/" \
              "$snapshot_dir/" 2>/dev/null || true
    else
        # 首次快照，完整复制
        log "INFO" "创建完整快照 (首次)"
        rsync -a "$source_mount/" "$snapshot_dir/" 2>/dev/null || true
    fi
    
    # 保存元数据
    echo "timestamp: $timestamp" > "$snapshot_dir/.zotero_snapshot"
    echo "source: $source" >> "$snapshot_dir/.zotero_snapshot"
    echo "message: $message" >> "$snapshot_dir/.zotero_snapshot"
    echo "date: $(date)" >> "$snapshot_dir/.zotero_snapshot"
    
    # 更新当前状态
    rsync -a --delete "$snapshot_dir/" "$REPO_BASE/$source/current/" 2>/dev/null || true
    
    # 记录提交
    echo "$timestamp|$source|$message|$(date '+%Y-%m-%d %H:%M:%S')" \
        >> "$REPO_BASE/$source/metadata/commits.log"
    
    log "SUCCESS" "$source 快照创建完成: $timestamp - $message"
    echo "快照ID: $timestamp"
    
    # 自动更新合并状态
    sync_merged_state
}

# 同步合并状态
sync_merged_state() {
    log "INFO" "更新合并状态"

    # 以 laptop/current 为基线先同步到 merged
    rsync -a --delete "$REPO_BASE/laptop/current/" "$REPO_BASE/merged/" 2>/dev/null || true

    # 准备检测 pc 与 merged 之间的冲突（文件存在且内容不同）
    local pc_current="$REPO_BASE/pc/current"
    local merged_root="$REPO_BASE/merged"
    local -a conflicts
    local tmp_exclude
    tmp_exclude=$(mktemp)
    local backup_dir="$REPO_BASE/merged/conflicts_backup/$(date '+%Y%m%d_%H%M%S')"
    mkdir -p "$backup_dir"

    if [ -d "$pc_current" ]; then
        while IFS= read -r -d '' pcfile; do
            rel="${pcfile#$pc_current/}"
            merged_file="$merged_root/$rel"
            if [ -f "$merged_file" ] && ! cmp -s "$pcfile" "$merged_file"; then
                conflicts+=("$rel")
            fi
        done < <(find "$pc_current" -type f -print0)
    fi

    if [ ${#conflicts[@]} -gt 0 ]; then
        echo "发现 ${#conflicts[@]} 个冲突文件，需要你选择保留哪边的版本："
        echo "冲突详情将记录在 $REPO_BASE/merged/.merge_conflicts"
        : > "$REPO_BASE/merged/.merge_conflicts"

        for rel in "${conflicts[@]}"; do
            pcfile="$pc_current/$rel"
            merged_file="$merged_root/$rel"
            echo "冲突: $rel" | tee -a "$REPO_BASE/merged/.merge_conflicts"
            echo "  Laptop: $(stat -c '%y %s' "$merged_file" 2>/dev/null)" | tee -a "$REPO_BASE/merged/.merge_conflicts"
            echo "  PC:     $(stat -c '%y %s' "$pcfile" 2>/dev/null)" | tee -a "$REPO_BASE/merged/.merge_conflicts"

            while true; do
                read -p "保留哪一方? (l)aptop / (p)c / (s)kip(保留当前merged) [l/p/s]: " -n 1 -r
                echo
                case "$REPLY" in
                    [Pp])
                        mkdir -p "$(dirname "$backup_dir/$rel")"
                        cp -a "$merged_file" "$backup_dir/$rel" 2>/dev/null || true
                        echo "$rel: kept pc" >> "$REPO_BASE/merged/.merge_conflicts"
                        break
                        ;;
                    [Ll])
                        echo "$rel: kept laptop" >> "$REPO_BASE/merged/.merge_conflicts"
                        echo "$rel" >> "$tmp_exclude"
                        break
                        ;;
                    [Ss])
                        echo "$rel: skipped (keep merged)" >> "$REPO_BASE/merged/.merge_conflicts"
                        echo "$rel" >> "$tmp_exclude"
                        break
                        ;;
                    *)
                        echo "请输入 l、p 或 s。"
                        ;;
                esac
            done
        done
    fi

    # 将 pc 的文件合并到 merged，排除用户选择保留 laptop/skip 的路径
    if [ -s "$tmp_exclude" ]; then
        rsync -a --delete --exclude-from="$tmp_exclude" "$pc_current/" "$merged_root/" 2>/dev/null || true
    else
        rsync -a --delete "$pc_current/" "$merged_root/" 2>/dev/null || true
    fi
    rm -f "$tmp_exclude" || true

    # 记录合并状态与冲突备份位置
    echo "last_merged: $(date '+%Y-%m-%d %H:%M:%S')" > "$REPO_BASE/merged/.merged_state"
    echo "laptop_version: $(ls -1t "$REPO_BASE/laptop/snapshots/" 2>/dev/null | head -1 || echo "none")" >> "$REPO_BASE/merged/.merged_state"
    echo "pc_version: $(ls -1t "$REPO_BASE/pc/snapshots/" 2>/dev/null | head -1 || echo "none")" >> "$REPO_BASE/merged/.merged_state"
    if [ -d "$backup_dir" ] && [ "$(ls -A "$backup_dir" 2>/dev/null)" ]; then
        echo "conflict_backup: $backup_dir" >> "$REPO_BASE/merged/.merged_state"
    fi
    log "INFO" "合并状态更新完成"
}

# 查看状态
cmd_status() {
    echo "=== Zotero版本控制系统状态 ==="
    echo "版本库: $REPO_BASE"
    echo ""
    
    # 检查挂载点
    echo "挂载点状态:"
    check_mount "$LAPTOP_MOUNT" "Laptop" > /dev/null && echo "  ✓ Laptop: $LAPTOP_MOUNT" || echo "  ✗ Laptop: 未挂载"
    check_mount "$PC_MOUNT" "PC" > /dev/null && echo "  ✓ PC: $PC_MOUNT" || echo "  ✗ PC: 未挂载"
    echo ""
    
    # 显示最新快照
    echo "最新快照:"
    local laptop_latest=$(ls -1t "$REPO_BASE/laptop/snapshots/" 2>/dev/null | head -1)
    local pc_latest=$(ls -1t "$REPO_BASE/pc/snapshots/" 2>/dev/null | head -1)
    
    if [ -n "$laptop_latest" ]; then
        local laptop_msg=$(grep "^message:" "$REPO_BASE/laptop/snapshots/$laptop_latest/.zotero_snapshot" 2>/dev/null | cut -d: -f2-)
        echo "  Laptop: $laptop_latest - ${laptop_msg:-无描述}"
    else
        echo "  Laptop: 无快照"
    fi
    
    if [ -n "$pc_latest" ]; then
        local pc_msg=$(grep "^message:" "$REPO_BASE/pc/snapshots/$pc_latest/.zotero_snapshot" 2>/dev/null | cut -d: -f2-)
        echo "  PC: $pc_latest - ${pc_msg:-无描述}"
    else
        echo "  PC: 无快照"
    fi
    echo ""
    
    # 检查差异
    echo "待同步变更:"
    cmd_diff "laptop" "current" 2>/dev/null | head -5 | while read line; do echo "  $line"; done
    cmd_diff "pc" "current" 2>/dev/null | head -5 | while read line; do echo "  $line"; done
}

# 查看差异
cmd_diff() {
    local source=$1  # "laptop" 或 "pc"
    local target=$2  # "current" 或 快照ID
    local source_mount=""
    
    case $source in
        "laptop") source_mount="$LAPTOP_MOUNT" ;;
        "pc") source_mount="$PC_MOUNT" ;;
        *) log "ERROR" "未知数据源: $source"; return 1 ;;
    esac
    
    if [ "$target" = "current" ]; then
        local compare_dir="$REPO_BASE/$source/current"
    else
        local compare_dir="$REPO_BASE/$source/snapshots/$target"
    fi
    
    if [ ! -d "$compare_dir" ]; then
        log "ERROR" "比较目录不存在: $compare_dir"
        return 1
    fi
    
    echo "[$source] 差异报告 ($(date '+%H:%M:%S')):"
    echo "源: $source_mount/"
    echo "目标: $compare_dir"
    echo ""
    
    # 使用rsync模拟差异检查
    rsync -avn --delete \
          "$source_mount/" \
          "$compare_dir/" 2>&1 | \
        grep -E "^[^.]|^sending|^deleting" | \
        sed 's/^/  /'
}

# 查看日志
cmd_log() {
    local source=${1:-"all"}
    
    echo "=== 提交日志 ==="
    
    if [ "$source" = "all" ] || [ "$source" = "laptop" ]; then
        echo "Laptop 提交记录:"
        if [ -f "$REPO_BASE/laptop/metadata/commits.log" ]; then
            cat "$REPO_BASE/laptop/metadata/commits.log" | while IFS='|' read id src msg date; do
                printf "  %s | %s | %s\n" "$id" "$date" "$msg"
            done
        else
            echo "  无记录"
        fi
        echo ""
    fi
    
    if [ "$source" = "all" ] || [ "$source" = "pc" ]; then
        echo "PC 提交记录:"
        if [ -f "$REPO_BASE/pc/metadata/commits.log" ]; then
            cat "$REPO_BASE/pc/metadata/commits.log" | while IFS='|' read id src msg date; do
                printf "  %s | %s | %s\n" "$id" "$date" "$msg"
            done
        else
            echo "  无记录"
        fi
    fi
}

# 恢复到指定快照
cmd_restore() {
    local source=$1  # "laptop" 或 "pc"
    local snapshot_id=$2
    local snapshot_dir="$REPO_BASE/$source/snapshots/$snapshot_id"
    local source_mount=""
    
    case $source in
        "laptop") source_mount="$LAPTOP_MOUNT" ;;
        "pc") source_mount="$PC_MOUNT" ;;
        *) log "ERROR" "未知数据源: $source"; return 1 ;;
    esac
    
    if [ ! -d "$snapshot_dir" ]; then
        log "ERROR" "快照不存在: $snapshot_id"
        echo "可用快照:"
        ls -1 "$REPO_BASE/$source/snapshots/" 2>/dev/null || echo "  无快照"
        return 1
    fi
    
    read -p "确认将 $source 恢复到快照 $snapshot_id? (y/N): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "取消恢复"
        return 0
    fi
    
    log "WARN" "正在恢复 $source 到快照: $snapshot_id"
    
    # 先备份当前状态（作为恢复前的安全点）
    local backup_id="backup_$(generate_snapshot_id)"
    rsync -a "$source_mount/" "$REPO_BASE/$source/snapshots/$backup_id/" 2>/dev/null || true
    echo "恢复前状态已备份到: $backup_id"
    
    # 执行恢复
    rsync -a --delete "$snapshot_dir/" "$source_mount/" 2>/dev/null || true
    
    log "SUCCESS" "$source 已恢复到快照: $snapshot_id"
    echo "恢复完成。原始状态备份在: $backup_id"
}

# 同步到另一台设备
cmd_sync() {
    local from=$1  # "laptop" 或 "pc"
    local to=$2    # "laptop" 或 "pc"
    
    if [ "$from" = "$to" ]; then
        log "ERROR" "不能同步到自身"
        return 1
    fi
    
    local from_mount=""
    local to_mount=""
    
    case $from in
        "laptop") from_mount="$LAPTOP_MOUNT" ;;
        "pc") from_mount="$PC_MOUNT" ;;
        *) log "ERROR" "未知源设备: $from"; return 1 ;;
    esac
    
    case $to in
        "laptop") to_mount="$LAPTOP_MOUNT" ;;
        "pc") to_mount="$PC_MOUNT" ;;
        *) log "ERROR" "未知目标设备: $to"; return 1 ;;
    esac
    
    # 检查挂载点
    check_mount "$from_mount" "$from" || return 1
    check_mount "$to_mount" "$to" || return 1
    
    log "INFO" "开始同步: $from → $to"
    
    # 检查目标设备Zotero是否正在运行（通过检查文件锁定）
    if lsof "$to_mount/zotero.sqlite" >/dev/null 2>&1; then
        log "ERROR" "目标设备 $to 的Zotero正在运行，请先关闭"
        return 1
    fi
    
    # 创建同步前快照
    cmd_commit "$from" "同步前快照: $from → $to"
    
    # 执行同步
    echo "正在同步变更..."
    rsync -av --progress --delete \
          "$from_mount/" \
          "$to_mount/" 2>/dev/null || true
    
    # 创建同步后快照
    cmd_commit "$to" "同步后快照: 从 $from 同步"
    
    # 更新合并状态
    sync_merged_state
    
    log "SUCCESS" "同步完成: $from → $to"
}

# 清理旧快照
cmd_clean() {
    local keep_days=${1:-30}
    log "INFO" "清理 $keep_days 天前的快照"
    
    for source in laptop pc; do
        local snapshots_dir="$REPO_BASE/$source/snapshots"
        if [ -d "$snapshots_dir" ]; then
            find "$snapshots_dir" -maxdepth 1 -type d -name "20*" -mtime +$keep_days | while read dir; do
                if [ -d "$dir" ]; then
                    echo "删除: $(basename "$dir")"
                    rm -rf "$dir"
                fi
            done
        fi
    done
    
    log "SUCCESS" "快照清理完成"
}

# 统计信息
cmd_stats() {
    echo "=== 存储统计 ==="
    
    for source in laptop pc merged; do
        local size=$(du -sh "$REPO_BASE/$source" 2>/dev/null | cut -f1)
        local snapshots_count=$(find "$REPO_BASE/$source/snapshots" -maxdepth 1 -type d -name "20*" 2>/dev/null | wc -l)
        
        printf "%-10s: %-8s (快照数: %d)\n" "$source" "$size" "$snapshots_count"
    done
    
    echo ""
    echo "最新合并状态:"
    if [ -f "$REPO_BASE/merged/.merged_state" ]; then
        cat "$REPO_BASE/merged/.merged_state" | sed 's/^/  /'
    fi
}

# ========== 主程序 ==========
main() {
    local command=${1:-"status"}
    
    case $command in
        "init")
            cmd_init
            ;;
        "commit")
            if [ $# -lt 2 ]; then
                echo "用法: $0 commit <laptop|pc> [描述]"
                exit 1
            fi
            cmd_commit "$2" "${3:-"自动提交"}"
            ;;
        "status")
            cmd_status
            ;;
        "diff")
            if [ $# -lt 2 ]; then
                echo "用法: $0 diff <laptop|pc> [current|快照ID]"
                exit 1
            fi
            cmd_diff "$2" "${3:-"current"}"
            ;;
        "log")
            cmd_log "${2:-"all"}"
            ;;
        "restore")
            if [ $# -lt 3 ]; then
                echo "用法: $0 restore <laptop|pc> <快照ID>"
                exit 1
            fi
            cmd_restore "$2" "$3"
            ;;
        "sync")
            if [ $# -lt 3 ]; then
                echo "用法: $0 sync <来源> <目标>"
                echo "示例: $0 sync laptop pc  # 从laptop同步到pc"
                exit 1
            fi
            cmd_sync "$2" "$3"
            ;;
        "clean")
            cmd_clean "${2:-30}"
            ;;
        "stats")
            cmd_stats
            ;;
        "help"|"--help"|"-h")
            show_help
            ;;
        *)
            echo "未知命令: $command"
            show_help
            exit 1
            ;;
    esac
}

show_help() {
    cat << EOF
Zotero版本控制系统 - 在两台Windows电脑间同步并管理Zotero数据

使用方法: $0 <命令> [参数]

命令:
  init                    初始化版本库
  commit <源> [描述]       创建快照 (源: laptop|pc)
  status                  查看当前状态
  diff <源> [目标]        查看差异 (目标: current|快照ID)
  log [源]                查看提交日志 (源: laptop|pc|all)
  restore <源> <快照ID>   恢复到指定快照
  sync <来源> <目标>       同步设备间数据
  clean [天数]            清理旧快照 (默认: 30天前)
  stats                   查看存储统计
  help                    显示此帮助信息

示例:
  $0 init                          # 初始化系统
  $0 commit laptop "添加新文献"    # 为laptop创建快照
  $0 diff laptop current           # 查看laptop当前状态与上次快照的差异
  $0 sync laptop pc                # 从laptop同步到pc
  $0 restore laptop 20231201_143000  # 恢复laptop到指定快照
  $0 stats                         # 查看存储使用情况

挂载点配置:
  Laptop: $LAPTOP_MOUNT
  PC: $PC_MOUNT
  版本库: $REPO_BASE

注意:
  1. 确保Windows共享文件夹已正确挂载
  2. 同步前请关闭Zotero程序
  3. 定期运行 clean 命令清理旧快照
EOF
}

# 运行主程序
main "$@"