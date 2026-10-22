# `dirty_snapshot.bin` 磁盘格式

本文件描述 QEMU [`-dirty-save`](../qemu-options.hx) 特性在 VM pause 时生成的
快照文件 `<workdir>/dirty_snapshot.bin` 的二进制布局。

- 生成：`softmmu/save_dirty.c` 的 `save_dirty_pause()`
- 消费：`softmmu/save_dirty.c` 的 `save_dirty_resume()`
- 生命周期：每次 pause 以 `O_TRUNC` 重新写入；resume 成功恢复后 `unlink` 删除。

## 1. 总览

文件分两部分：**一个固定大小的文件头** + **若干块记录**（每块一条，数量由
`block_count` 给出）。字节序为主机原生（主流平台为小端）。所有整数均为无符号。

```
+------------------------------------------------------------------+
|  File Header (固定 32 字节)                                       |
+------------------------------------------------------------------+
|  Block Record 0   (变长: idstr 长度不定 + slot_map + data)         |
|  Block Record 1                                                    |
|  ...                                                               |
|  Block Record block_count-1                                        |
+------------------------------------------------------------------+
```

## 2. 文件头（32 字节）

对应 `softmmu/save_dirty.c` 里的 `SnapHeader`。

| 字段 | 类型 | 大小 | 说明 |
|------|------|------|------|
| `magic` | u32 | 4 | 魔数 `0x53445151`，无特殊含义，仅用于识别文件身份 |
| `version` | u32 | 4 | 格式版本，当前 `1` |
| `block_count` | u32 | 4 | 本文件中实际写入的块记录条数 |
| `pad` | u32 | 4 | 对齐填充（固定为 0）|
| `quota` | u64 | 8 | 本次 pause 用的配额上限（0 = 不限）|
| `total_data` | u64 | 8 | 所有块数据区的字节总数（诊断/完整性参考）|

> 写入顺序：`save_dirty_pause` 先写一个占位头，写完全部块后 `lseek(0)` 重写，
> 使 `block_count`/`total_data` 为最终真实值。
>
> **只写"有脏页要保存"的块**：循环里 `nsave == 0` 的块直接 `continue`，不产生任何
> 块记录。因此 `block_count` 实际等于"保存了 ≥1 个脏页的块数"，文件里不存在空记录。

## 3. 块记录（每条，变长）

对应 `save_dirty_pause` 中 per-block 的 `write_all` 序列。

```
+--------+------------------+---------+---------+---------+----------+------+
| idl    | idstr            | used    | psz     | nsave   | slot_map | data |
| u32(4) | idl 字节          | u64(8)  | u64(8)  | u64(8)  | 变长      | 变长 |
+--------+------------------+---------+---------+---------+----------+------+
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `idl` | u32 | `idstr` 的字节长度 |
| `idstr` | `idl` 字节 | 块名（`rb->idstr`，文件内**不带 NUL 终止符**）。resume 用它 `strcmp` 匹配当前 `RAMBlock` |
| `used` | u64 | 块已用长度（`rb->used_length`）|
| `psz` | u64 | 本块保存粒度 = host 页大小（`qemu_ram_pagesize(rb)`，普通 4K / hugepage 2M）|
| `nsave` | u64 | 本块实际保存的 host 页数 |
| `slot_map` | `(slots+7)/8` 字节 | 槽位图，`slots = used / psz` 位；第 i 位 = 1 表示该槽已保存 |
| `data` | `nsave * psz` 字节 | 打包的页数据，按槽下标**升序**排列，第 j 个数据页对应第 j 个置位槽 |

### 3.1 `slot_map`
- 每个 bit 对应一个"保存单元"（一个 host 页 = `psz` 字节）。
- resume 按它知道哪几个槽有数据：遍历 `slot = 0..slots-1`，`slot_map[slot/8]` 的第
  `slot%8` 位为 1 时，从 `data` 依次读 `psz` 字节写回 `rb->host + slot*psz`。
- 只有被保存的槽会被置位；**超过 quota 的脏页不进 `slot_map`、也不进 `data`**。

### 3.2 `data` 与偏移
由于 `data` 按槽升序顺序打包，第 k 个置位槽的数据紧跟在第 k-1 个置位槽之后，
resume 只需线性读过去即可，无需随机偏移表。

## 4. 示例（以 `-dirty-save dir=/tmp/ds,quota=64M` + `-m 128` 为例）

```
File Header:
  magic=0x53445151  version=1  block_count=4  pad=0  quota=64MiB  total_data=200704

Block Record 0:        idstr="pc.ram"   used=0x6000000  psz=4096  nsave=45
                       slot_map: 共 45 bits 置位
                       data: 45*4096 = 184320 字节
Block Record 1:        idstr="/rom@etc/acpi/tables"  used=...  psz=4096  nsave=2
                       data: 2*4096 = 8192 字节
Block Record 2:        idstr="/rom@etc/table-loader" ... nsave=1
                       data: 4096 字节
Block Record 3:        idstr="/rom@etc/acpi/rsdp"    ... nsave=1
                       data: 4096 字节
```

> total_data = (45+2+1+1) × 4096 = 200704 字节，与 header 的 `total_data` 一致。
> `pc.bios` / `pc.rom` 等只读块因为没有脏页（`nsave==0`），**完全不出现在文件里**。