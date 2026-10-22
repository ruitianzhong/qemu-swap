# 实现计划：pause 时保存并 drop 脏页 / resume 时恢复

目标：在 QEMU 6.2.0 中添加一个特性，当 VM 被 pause 时，把它的"脏"内存
（guest 写入、触发 COW 的 file-backed MAP_PRIVATE 页）保存为文件，并对这些
页调用 `madvise(MADV_DONTNEED)` 释放 RSS；resume 时从文件读回并写回内存。
命令行为该功能提供三个开关：使能、工作目录、脏页保存配额上限。

相关文档：`vm_swap/README.md`（1.1-1.2 节，原始需求与实现映射）。

---

## 1. 总体设计

**脏页检测方案（已定）：内核 soft-dirty 机制，通过 `/proc/<pid>/pagemap` + `/proc/<pid>/clear_refs`。**
不再使用 QEMU 的脏日志机器（无 `MemoryListener`、无 `memory_global_dirty_log_start`、无
`snapshot_and_clear_dirty`）。

- **原理**：向 `/proc/<pid>/clear_refs` 写 `4` 清空全体 soft-dirty 位；此后 guest 写某个 host 页
  会让该页 PTE 置软脏位。pause 时读 `/proc/<pid>/pagemap`，`bit55(PAGEMAP_SOFT_DIRTY)` 置位的
  host 页即为"guest 自上次清位以来写过"的页 = 我们想保存的 COW 页。
- **粒度天然是 host 页** → 直接喂给 `madvise`，**无 4K vs host 页对齐问题**（neither 迁移脏位图，
  也无需 `QEMU_MADV_DONTNEED` 的 4K 聚合）。
- **启动时探测**：写 `clear_refs 4` 失败且 `errno==EINVAL` → 内核未编 `CONFIG_MEM_SOFT_DIRTY`；
  `EACCES/EPERM` → 权限不足。两者均 `error_fatal("...")` 退出，**不做回退**（选型：保持单一路径简单）。
- **触发点（hook 原则：pause 后置、resume 前置）**：
  - **pause（保存 + drop）**：挂在该 pause 周期原有逻辑全部完成之后 —— `do_vm_stop()`
    （`softmmu/cpus.c:254`）函数末尾，`bdrv_drain_all()/bdrv_flush_all()`（第 268-269 行）之后、
    `return ret` 前。此时 vCPU 已停、块层已 flush，内存静止。仅当状态为 `RUN_STATE_PAUSED`
    （`stop`/HMP `stop`）且功能开启时执行；`RESTORE_VM`（`loadvm`）等自动排除。
  - **resume（恢复）**：`vm_start()`（`softmmu/cpus.c:695`）函数最开头、`vm_prepare_start()` 之前
    load 脏页；恢复写回后内部再 `clear_refs`（避免写回动作把刚恢复的页当 dirty 留到下次 pause）。
- **配额（quota）**：跨所有块累计"保存的 host 页字节数"，达 quota 即停止新增保存。超过配额的脏页
  不保存、不 drop（留驻内存），保证速度和内存的 tradeoff，且 resume 一定能完整写回。

---

## 2. 新增/改动文件清单

| 文件 | 动作 | 内容 |
|------|------|------|
| `softmmu/save_dirty.c` | 新增 | 选项、soft-dirty 探测/pagemap 读取、保存/恢复文件 I/O、madvise |
| `include/sysemu/save_dirty.h` | 新增 | `save_dirty_init()`、`save_dirty_pause()`、`save_dirty_resume()` |
| `qemu-options.hx` | 修改 | 新增 `-dirty-save dir=PATH[,quota=N]` 选项及文档 |
| `softmmu/vl.c` | 修改 | 解析新选项、machine 就绪后调 `save_dirty_init()` |
| `softmmu/cpus.c` | 修改 | `do_vm_stop` 末尾调 `save_dirty_pause()`；`vm_start` 开头调 `save_dirty_resume()` |
| `softmmu/meson.build` | 修改 | 添加 `files('save_dirty.c')` |

无需 MemoryListener / 全局脏日志 / 任何内存子系统注册。

---

## 3. 命令行选项

### 3.1 选项形态
```
-dirty-save dir=PATH[,quota=N]
```
- `dir=PATH`：保存目录，也是使能开关（缺省则不使能）。
- `quota=N`：脏页保存上限字节数，可带 `K/M/G`；缺省 0 = 不设限。

> 需求三项映射：出现 `-dirty-save`=使能、`dir=`=工作目录、`quota=`=配额。

### 3.2 qemu-options.hx
新增 `DEF("dirty-save", HAS_ARG, QEMU_OPTION_dirty_save, ...)` + `SRST/ERST` 文档。
`QEMU_OPTION_*` 枚举由 meson 从 hx 自动生成，无需手改枚举。

### 3.3 vl.c 解析
- `case QEMU_OPTION_dirty_save:` 用 `keyval_parse_into()` 解析 optarg，取出 `dir`/`quota`，
  存入 `save_dirty` 全局配置（`save_dirty_parse_options()`）。
- machine 初始化完成、VM 未启动（`qemu_machine_creation_done()` 附近）调 `save_dirty_init()`
  ：`mkdir -p workdir`、**探测 soft-dirty 支持**（不支持则 fatal）、打开 `/proc/self` 的
  pagemap/clear_refs fd、执行首次 `clear_refs 4` 建立基线。

---

## 4. 核心模块 `softmmu/save_dirty.c`

### 4.1 全局状态
```c
static bool      dirty_save_enabled;
static char     *dirty_save_dir;
static uint64_t  dirty_save_quota;   /* 0 = 不限 */
static int       pagemap_fd  = -1;   /* /proc/<pid>/pagemap  */
static int       clearrefs_fd = -1;  /* /proc/<pid>/clear_refs */
static size_t    host_page_size;     /* qemu_real_host_page_size() */
static const uint64_t ENTRY_SOFT_DIRTY = 1ULL << 55;
```
无需 per-block 常驻结构：每次 pause 用 `RAMBLOCK_FOREACH` 现遍历，现读 pagemap。

### 4.2 soft-dirty 探测与清位（arm）
```c
void save_dirty_init(void)
{
    if (!dirty_save_enabled) return;
    mkdir(dirty_save_dir, 0700);
    char *pid = ...;  /* 用 getpid() 拼 /proc/<pid>/pagemap 与 /proc/<pid>/clear_refs */

    pagemap_fd   = open("/proc/<pid>/pagemap", O_RDONLY);
    clearrefs_fd = open("/proc/<pid>/clear_refs", O_WRONLY);
    /* 任一打开失败 → error_fatal(权限) */

    if (write(clearrefs_fd, "4\n", 2) < 0) {
        if (errno == EINVAL) error_fatal("kernel lacks CONFIG_MEM_SOFT_DIRTY");
        else                error_fatal("clear_refs: %s", strerror(errno));
    }
    host_page_size = qemu_real_host_page_size();
}
/* 每次读/写/恢复后调用，重新建立"仅记录自此刻起的新写"基线 */
static void arm_soft_dirty_track(void) { 
    if (lseek(clearrefs_fd, 0, SEEK_SET) == 0)
        (void)write(clearrefs_fd, "4\n", 2);
}
```
> 每次 `arm` 都是"清空已累积的软脏位"。init / 每次 pause 保存+drop 后 / 每次 resume 写回后各一次。

### 4.3 pause：`save_dirty_pause(RunState state)`

```c
void save_dirty_pause(RunState state)
{
    if (!dirty_save_enabled || state != RUN_STATE_PAUSED) return;

    char *path = g_strdup_printf("%s/dirty_snapshot.bin", dirty_save_dir);
    int fd = open(path, O_WRONLY|O_CREAT|O_TRUNC, 0600);

    uint64_t saved_bytes = 0;
    RAMBLOCK_FOREACH(rb) {
        if (!dirty_save_block_owned(rb)) continue;

        /* 读本块 pagemap：按 4K 粒度整块读入缓冲（一次 pread 大块） */
        uint8_t *buf = pread_block_pagemap(rb);   /* 长度 = (used_length>>12)*8 字节 */
        int *slot_dirty = resolve_hostpage_dirty(buf, rb->host, rb->used_length);

        /* 逐 host 页槽：dirty 且 quota 未满 → 保存整槽 + 记入待 drop 集合 */
        for (i 遍历 dirty 槽, 块内偏移 off = i * page_size(rb)) {
            if (saved_bytes + ps > dirty_save_quota) break;   /* 超配额：不保存不 drop */
            pwrite(fd, 块元数据描述(off), ...);
            pwrite(fd, rb->host + off, ps);     /* 整 host 页数据 */
            saved_bytes += ps;
            /* 记下 host 区间 [rb->host+off, +ps)，供下方聚合 drop */
        }
        g_free(buf);
    }

    /* 聚合连续保存过的 host 区间 → madvise，见下方伪码 */
    madvise_saved_slots();

    close(fd); g_free(path);
    arm_soft_dirty_track();   /* drop 之后清位，让下次 pause 只反映新写 */
}
```

**pagemap 读取**（块内 4K 页 `idx` 对 buffer 下标 `idx`）：
```c
static uint8_t *pread_block_pagemap(RAMBlock *rb)
{
    size_t n = QEMU_ALIGN_UP(rb->used_length, host_page_size) / host_page_size;
    size_t bytes = n * 8;
    uint8_t *buf = g_malloc0(bytes);
    off_t  file_off = ((uintptr_t)rb->host >> PAGE_SHIFT) * 8;  /* PAGE_SHIFT=12 */
    if (pread(pagemap_fd, buf, bytes, file_off) != bytes)
        warn();   /* 退回 readonly：本次跳过该块 */
    return buf;
}
/* 判断某 host 页槽 dirty：其任一 4K 子项 soft-dirty 即 dirty
   （hugepage 下 PMD 共享一个软脏位，OR 一致 → 整槽 sink 正确） */
static bool slot_dirty(buf, base_va, slot_idx, page_size) {
    for (k = slot_idx*(page_size/4K); k < (slot_idx+1)*(page_size/4K); k++)
        if (le64(buf + k*8) & ENTRY_SOFT_DIRTY) return true;
    return false;
}
```

**madvise & 对齐**（沿用之前结论：`rb->host` 是文件 mmap 基址 + host 页对齐聚合）：
```c
size_t ps = qemu_ram_pagesize(rb);            /* 普通 4K / hugepage 2M, physmem.c:1797 */
uint8_t *h = rb->host;                        /* file-backed 时 file_ram_alloc 的 mmap 基址, physmem.c:2095 */

uint8_t *rs = NULL, *re = NULL; bool in = false;
for (已保存槽 i) {
    uint8_t *p = h + i*ps;
    if (!in)                    { rs = re = p; in = true; }
    else if (p == re)           { re = p + ps; }          /* 相邻 → 合并 */
    else { qemu_madvise(rs, re - rs, QEMU_MADV_DONTNEED); rs = p; re = p + ps; }
}
if (in) qemu_madvise(rs, re - rs, QEMU_MADV_DONTNEED);
```
因为保存本身就按 host 页 `ps` 对齐做单元，起点/长度天然对齐，**不需要 `QEMU_ALIGN_DOWN/UP`**——只有连续槽合并。

要点：
- 时机：`do_vm_stop()` 末尾（`bdrv_drain_all/flush_all` 后），内存静止稳定。
- 保存整 host 页 = 与 madvise 粒度一致；hugepage 下整个 COW 页都归我们，整页保存/恢复正确。
- 先 `pwrite` 落盘成功，才 drop；madvise 失败仅告警。
- 超配额槽不保存不 drop（留在内存）。

### 4.4 resume：`save_dirty_resume(void)`

```c
void save_dirty_resume(void)
{
    if (!dirty_save_enabled) return;
    if (!runstate_check(RUN_STATE_PAUSED)) return;   /* 防"非暂停态还 load 旧快照" */
    char *path = ...; if (access(path, F_OK) < 0) { g_free(path); return; } /* 没 pause 过 → no-op */

    int fd = open(path, O_RDONLY);
    /* 读 header → 逐块：按 "槽位图 + 打包数据" 恢复 */
    foreach block record:
        memcpy(rb->host + slot_idx*page_size, data_ptr, page_size);
    close(fd); unlink(path);

    arm_soft_dirty_track();   /* 恢复写回本身会置脏，清位避免下次 pause 误抓这些刚恢复的页 */
}
```
- 时机：`vm_start()` 函数最开头、`vm_prepare_start()` 之前，runstate 未翻转为 RUNNING、vCPU 未跑就完成写回。
- 写回 `MAP_PRIVATE` 会重新触发 COW 分配匿名页，内容来自快照文件。

### 4.5 快照文件格式 `<workdir>/dirty_snapshot.bin`
```
[Header]
  magic  : uint32 = 0x53445151 "QDSN"
  version: uint32 = 1
  block_count : uint32
  quota  : uint64
  total_data  : uint64
[Per block]
  idstr_len : uint32, idstr : bytes
  used_length : uint64
  page_size(pm洁度) : uint64      ← 保存粒度 = host 页 = qemu_ram_pagesize(rb)
  saved_slots : uint64           ← 本块实际保存的 host 页数
  slot_map    : (used_length/page_size) 位的位图，置位=该槽已存（on-disk 元数据）
  [data] : saved_slots * page_size 字节，按 slot_map 置位顺序打包
```
- slot_map 即"页选集元数据"，与运行期数据结构同构（给一个 bit 数组），resume 按其置位逐槽读数据写回。
- resume 按 `idstr` 匹配 `RAMBlock`，`used_length/page_size` 校验；匹配不到丢弃/报错。
- 每次 pause `O_TRUNC` 覆盖同一个文件；resume 读完 `unlink` 删除，避免 stale。

---

## 5. 入侵点修改细节

### 5.1 `softmmu/cpus.c`

hook 位置原则：**pause 在原有暂停逻辑完全跑完后再保存；resume 在原有恢复逻辑开始前先 load**。

- `do_vm_stop()`（254 行）：函数**末尾**、`bdrv_drain_all()`(268)/`bdrv_flush_all()`(269) 之后、
  `return ret;`(272) 之前插入 `save_dirty_pause(state);`：
  ```c
      bdrv_drain_all();
      ret = bdrv_flush_all();
      trace_vm_stop_flush_all(ret);

      save_dirty_pause(state);   /* 新增：原有 pause 全部完成后才保存脏页 */
      return ret;
  ```
  > `do_vm_stop` 还服务 SHUTDOWN/DEBUG/RESTORE_VM 等状态，由 `save_dirty_pause` 以
  > `state != RUN_STATE_PAUSED` 过滤；`hmp_loadvm` 走 `RESTORE_VM` 不误触发。

- `vm_start()`（695 行）：最开头 load，`vm_prepare_start()` 之前：
  ```c
  void vm_start(void)
  {
      save_dirty_resume();   /* 新增：在原有 resume 逻辑开始前先 load 脏页 */
      if (!vm_prepare_start()) {
          resume_all_vcpus();
      }
  }
  ```
  > `vm_start()` 是 resume 集中入口（`qmp_cont`→`monitor/qmp-cmds.c:155`、`hmp_loadvm` 恢复路径），
  > 放这覆盖所有 resume。`save_dirty_resume` 内部 guard `RUN_STATE_PAUSED` + 快照存在。

### 5.2 `softmmu/vl.c`
- `case QEMU_OPTION_dirty_save:` 解析 optarg 存入配置。
- machine ready 后调 `save_dirty_init()`（建目录 + 探测 + 建基线）。

### 5.3 `softmmu/meson.build`
`softmmu_ss.add(...)` 处加入 `files('save_dirty.c')`。

---

## 6. 头文件 `include/sysemu/save_dirty.h`
```c
void save_dirty_parse_options(const char *optstr, Error **errp);
void save_dirty_init(void);    /* 探测 soft-dirty + 建 workdir + 建基线 */
void save_dirty_pause(RunState state);
void save_dirty_resume(void);
```

---

## 7. 边界/注意事项
1. **内核依赖探测**：`clear_refs 4` 失败 `EINVAL`（缺 `CONFIG_MEM_SOFT_DIRTY`）或权限错，均
   `error_fatal` 退出；无运行时回退。
2. **权限**：读 `/proc/<pid>/pagemap`、写 `clear_refs` 需 `CAP_SYS_ADMIN` 或自有进程；多数部署 root
   无碍，需在启动探测时确认。
3. **自写置脏**：QEMU 内部写 RAM（模拟设备 DMA）也置 soft-dirty → 过估计（可接受）。**resume 写回
   动作会置脏**，务必 `vm_start` 里恢复后 `arm_soft_dirty_track()` 清一户，才不破坏 quota 冷页语义。
4. **配额**：只保存并 drop `<= quota` 的 host 页；超出留驻。resume 只写回已存槽。只有被保存的槽才
   进 slot_map，配额语义自动成立。
5. **页对齐**：保存粒度即 host 页 = madvise 粒度，天然对齐；仅做相邻槽合并。
6. **挂页/swap**：soft-dirty 对非 present 页忽略，swap 出去的脏页不误 drop（madvise 对非 present 亦是
   no-op），内容留在 swap 不丢。
7. **首次 boot / 无快照**：resume 无文件 → no-op。
8. **多次 stop 未 cont**：快照每次覆盖，只保留最后一次；串行 stop→cont 符合需求模型。
9. **多 VM 共享后端文件**：只在单 VM 内 drop 自己的 COW 页，不影响其他 VM 共享只读页。
10. **性能**：每次 pause 整块读 pagemap（8B/4K ≈ used_length/512，8GB≈16MB），pause 低频可接受；
    同步文件 I/O 在 iothread 主循环，本期保持简单。

---

## 8. 实施顺序（建议）
1. 新增 `save_dirty.h` / `save_dirty.c`：只做 `parse_options` + 探测 + `init`（建目录、探测、基线），
   加进 `meson.build`，跑通构建。
2. 改 `qemu-options.hx` + `vl.c`：选项能解析、help 能打印、`init` 能被调用。
3. 实现 pause：`pread_block_pagemap` → 解析 soft-dirty → 保存 host 页 → 聚合 madvise → arm。
4. 实现 resume：读文件 → 逐槽 `memcpy` 写回 → arm。
5. 测试：带 file-back RAM 起 VM→写内存→`stop`→查 RSS 下降、快照生成→`cont`→内存内容一致、无崩溃；
   另测不含 `CONFIG_MEM_SOFT_DIRTY` 的内核下启动能正确 fatal。