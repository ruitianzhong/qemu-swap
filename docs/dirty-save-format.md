# `dirty_snapshot.bin` 磁盘格式（v2）

本文件描述 QEMU [`-dirty-save`](../qemu-options.hx) 特性在 VM pause 时生成的
快照文件的二进制布局。默认路径为 `<dir>/dirty_snapshot.bin`；可通过
`-dirty-save ...,file=PATH` 指定其它路径（PATH 必须包含文件名，且其父目录
必须已存在）。

- 生成：`softmmu/save_dirty.c` 的 `save_dirty_pause()`
- 消费：`softmmu/save_dirty.c` 的 `save_dirty_resume()`
- 触发：命令行 `-dirty-save` 启用能力，且本次 pause 通过 QMP `stop` 传入
  `enable-save-dirty=true`；普通 pause 不生成快照
- 生命周期：每次 pause 以 `O_TRUNC` 重新写入；resume 成功恢复后 `unlink` 删除
- 版本：当前为 **v2**，不兼容 v1；启动时 `save_dirty_init()` 会删除同名旧快照

## 1. 总览

文件由 **4 KiB 对齐的头部/块元数据** 与 **按页紧凑排列的数据区** 组成：

```text
+--------------------------------------------------+ 0
| SnapHeader (4096 字节，尾部补 0)                  |
+--------------------------------------------------+ 4096
| BlockMeta + idstr + slot_map + padding            |
|   → 补齐到 4096 边界                              |
+--------------------------------------------------+
| data: nsave * psz                                 |
+--------------------------------------------------+
| 下一个 BlockMeta ...                              |
+--------------------------------------------------+
```

字节序为主机原生（主流平台为小端）。所有整数均为无符号。

## 2. 文件头（4096 字节）

对应 `softmmu/save_dirty.c` 里的 `SnapHeader`。

| 字段 | 类型 | 大小 | 说明 |
|------|------|------|------|
| `magic` | u32 | 4 | 魔数 `0x53445151`，仅用于识别文件身份 |
| `version` | u32 | 4 | 格式版本，当前 `2` |
| `align` | u32 | 4 | 对齐粒度，固定 `4096` |
| `block_count` | u32 | 4 | 本文件中实际写入的块记录条数 |
| `quota` | u64 | 8 | 本次 pause 实际使用的配额上限（0 = 不限；QMP `quota` 可覆盖 CLI 值）|
| `total_data` | u64 | 8 | 所有块数据区的字节总数（不含元数据与 padding）|
| `flags` | u64 | 8 | bit0 = 本次是否使用 O_DIRECT（仅诊断）|
| `reserved` | u8[] | 4064 | 补 0，保证 header 大小为 4096 |

写入顺序：先写占位头（`block_count=0`），写完全部块后重写最终 header，
然后执行唯一一次 `fsync()`。

> **只写"有脏页要保存"的块**：`nsave == 0` 的块不产生块记录。因此
> `block_count` 等于"保存了 ≥1 个脏页的块数"。

## 3. 块记录

### 3.1 固定元数据（48 字节）

对应 `BlockMeta`：

| 字段 | 类型 | 说明 |
|------|------|------|
| `idl` | u32 | `idstr` 的字节长度（不含 NUL）|
| `map_bytes` | u32 | `slot_map` 字节数，`(slots + 7) / 8` |
| `used` | u64 | 块已用长度（`rb->used_length`）|
| `psz` | u64 | 保存粒度 = host 页大小（当前按 4K）|
| `nsave` | u64 | 本块实际保存的页数 |
| `data_bytes` | u64 | 数据区长度，必须等于 `nsave * psz` |
| `reserved` | u64 | 固定 0 |

固定元数据之后依次是：

```text
idstr[idl]         块名（不带 NUL）
slot_map[map_bytes] 槽位图
padding             补 0 到 4096 边界
```

数据区起点为 `align_up(元数据起点 + 48 + idl + map_bytes, 4096)`。

### 3.2 `slot_map`

- 每个 bit 对应一个保存单元（一个 host 页 = `psz` 字节）；
- 第 i 位 = 1 表示该槽已保存；
- `nsave` 必须等于 `slot_map` 中置位的数量；
- 超过 quota 的脏页不进 `slot_map`、也不进数据区。

### 3.3 数据区

- 共 `nsave * psz` 字节，按槽下标升序紧凑排列；
- 第 k 个置位槽的数据紧跟在第 k-1 个置位槽之后；
- `psz` 为 4K，数据长度天然是 4K 的倍数，因此下一块的元数据起点保持
  4K 对齐；
- 写入使用 `pwritev` 批量提交（每批最多 `-dirty-save batch=` 指定的字节数，
  默认 4 MiB，最多 256 个 iovec）；不足一批的尾批按实际长度写入，不补 0；
- 启用 O_DIRECT 时，文件偏移、iovec 地址和长度都按 4K 对齐。

## 4. 示例

```text
SnapHeader:
  magic=0x53445151  version=2  align=4096  block_count=4
  quota=0  total_data=200704  flags=1

Block Record 0:        idstr="mem0"  used=0x100000000  psz=4096  nsave=45
                       map_bytes=131072  data_bytes=184320
                       data: 45*4096 = 184320 字节
Block Record 1:        idstr="/rom@etc/acpi/tables"  nsave=2 ...
Block Record 2:        idstr="/rom@etc/table-loader" nsave=1 ...
Block Record 3:        idstr="/rom@etc/acpi/rsdp"    nsave=1 ...
```

`total_data = sum(data_bytes)`，与 header 中的 `total_data` 一致。
