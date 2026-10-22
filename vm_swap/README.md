# vm_swap 使用说明

本目录是从 `EmuFork/vm_swap.py` 裁剪/移植过来的 QEMU fork + dirty-save 测试脚本，
用于验证本仓库的 `-dirty-save`（vm-swap）特性。

## 1. 它做了什么

1. 启动一个模板 VM（`QEMUTemplate`，idx 0），内存用 `memory-backend-file,share=on`。
2. 在 guest 内启动 `server_qemu`，然后 `stop` 暂停、`migrate`（`x-ignore-shared`）
   把设备/CPU 状态写入 `vm_state`，最后退出模板进程。
3. fork 出 4 个子 VM（`QEMUChild`，idx 1..4）：
   - 共享同一个 `vm_mem` 文件，但以 `share=off` 映射，实现按页 COW；
   - 每个子 VM 有独立的 qcow2 覆盖层磁盘和独立管理 tap；
   - `-S -incoming defer` 启动后通过 QMP `migrate-incoming` 载入 `vm_state`，再 `cont`。
4. `--mode swap --dirty-save` 时，每个子 VM 额外带
   `-dirty-save dir=<common dir>[,quota=...],file=<per-VM file>`（只启用能力）；
   pause 时 harness 还会通过 QMP 传 `enable-save-dirty=true`（以及可选的 quota），
   QEMU 才会把 guest 脏页写入快照文件并 `MADV_DONTNEED` 释放 RSS；resume 时从该
   文件恢复。所有子 VM 的快照文件位于同一目录
   `<output_dir>/dirty_save/`，文件名固定为 `dirty_<idx>.bin`。QMP stop 不带
   `enable-save-dirty=true` 时不会保存，也不会 drop。
5. 依次做 `PAUSE_RESUME_ROUNDS`（默认 4）轮 pause/resume，每轮在 guest 内执行命令。
   每轮会在 pause 前、pause 后、resume 后各执行一次 `free -b`，打印宿主机 used
   内存及 `reclaimed` / `vs before pause` 差值（GiB 或 MiB）；所有轮次结束后
   再打印 `memory summary` 平均汇总。最后关闭所有子 VM 并清理 tap。

### 1.1 原始需求

本仓库实现 vm-swap 的出发点：

- QEMU 本身支持 VM 的 pause/resume，需要在 **pause** 时处理 dirty 内存：
  - dirty 内存指通过 `MAP_PRIVATE` 方式 mmap 的 file-backed memory，不同 VM
    通过这种映射共享未修改的部分；
  - 把 dirty 页保存为一个文件；
  - 保存后对对应 mmap 区间调用 `madvise(MADV_DONTNEED)`，释放宿主内存。
- 在 **resume** 时把相应脏页从文件中读出并写回 guest 内存。
- 命令行需要提供三个能力：
  - 使能该功能；
  - 指定当前 VM 的工作目录，保存的文件都放在该目录；
  - 指定最多保存的内存大小（quota），在速度和内存节省之间取舍。
- `madvise` 只 drop 包含脏页的区间，而不是整块内存。
- 实现前需要确认 QEMU 如何添加命令行选项，并定位 pause/resume 的实现位置。

### 1.2 需求到实现的映射

- 使能开关与工作目录：`-dirty-save dir=PATH`，出现该选项即启用能力。
- 保存上限：`-dirty-save dir=PATH,quota=N`，`N` 支持 `K/M/G`；省略或 0 表示不限。
- 快照文件：默认 `<dir>/dirty_snapshot.bin`；可用 `file=PATH` 覆盖（PATH 包含
  文件名，父目录必须已存在）。
- QMP 控制：`stop` 的 `enable-save-dirty=true` 才触发保存；`quota` 可覆盖本次 pause
  的配额。详见 4.2。
- 脏页检测：内核 soft-dirty（`/proc/<pid>/pagemap` + `/proc/<pid>/clear_refs`），
  粒度为 host page。
- pause 保存/回收：`softmmu/cpus.c` 的 `do_vm_stop()` 调用
  `save_dirty_pause()`，由 `softmmu/save_dirty.c` 写文件并 `madvise`。
- resume 恢复：`softmmu/cpus.c` 的 `vm_start()` 调用 `save_dirty_resume()`。
- 选项解析与初始化：`softmmu/vl.c`；帮助文本在 `qemu-options.hx`。
- 快照文件布局：见 `docs/dirty-save-format.md`。

## 2. 前置条件

- Linux + KVM；创建 tap、访问 `/dev/kvm`、ssh 登录 guest，通常需要 **root** 运行。
- 本仓库的 QEMU 已构建并安装到 `/opt/qemu-exp`（`vm_swap.py` 顶部的 `QEMU_PATH`）。
- 构建依赖：仓库根目录执行 `./vm_swap/install.sh`（或在 `vm_swap/` 内执行
  `./install.sh`）安装 apt 包。
- 宿主机工具：`ip`、`ssh`、`scp`、`make`、`g++`、`gcc`。
- 运行目录中需要有：
  - `base.qcow2`：模板磁盘镜像（guest 已配置好 ssh，root 可登录）；
  - `id_rsa`：对应 guest `authorized_keys` 的私钥（脚本里按相对路径 `id_rsa` 使用）。

## 3. 构建

详细说明见同目录下的 `BUILD.md`，configure 选项见 `COMPILE_OPTIONS.md`。

### 3.1 构建 QEMU

在仓库根目录执行：

```bash
./vm_swap/install.sh       # 首次构建时安装依赖（可跳过）
./vm_swap/build.sh         # configure（如需要）+ make + make install
```

也可以在 `vm_swap/` 目录内执行：

```bash
cd vm_swap
./install.sh
./build.sh
```

默认 prefix 是 `/opt/qemu-exp`，与 `vm_swap.py` 的 `QEMU_PATH` 一致。常用选项
（以下命令均在仓库根目录执行）：

```bash
./vm_swap/build.sh --clean         # 删掉 build/ 后从头构建
./vm_swap/build.sh --no-install    # 只编译不安装
PREFIX=/opt/qemu-exp TARGETS=x86_64-softmmu JOBS=16 ./vm_swap/build.sh
./vm_swap/build.sh --help
```

如果 `/opt/qemu-exp` 不可写，脚本会提示：

```bash
sudo make -C build install
```

验证安装的 QEMU 带 dirty-save：

```bash
/opt/qemu-exp/bin/qemu-system-x86_64 --help 2>&1 | grep dirty-save
# 期望包含：-dirty-save dir=PATH[,quota=B]
```

如果使用其它 prefix，修改 `vm_swap.py` 开头的 `QEMU_PATH`。

### 3.2 agent 二进制

`agent/client_qemu`、`agent/server_qemu` 会在脚本启动时自动检查：
缺失、不可执行或源码比二进制新时，自动执行 `make -C vm_swap/agent`。
也可手动构建：

```bash
make -C vm_swap/agent
```

使用 `--dirtier` 时，`dirtier/dirtier` 也会按同样的规则自动检查：
缺失、不可执行或 `dirtier.c` 比二进制新时，自动执行 `make -C vm_swap/dirtier`。
也可手动构建：

```bash
make -C vm_swap/dirtier
```

## 4. 运行

在包含 `base.qcow2` 和 `id_rsa` 的目录下运行（例如
`/home/rtzhong/qemu-playground`）：

```bash
cd /home/rtzhong/qemu-playground

# swap 模式 + dirty-save，配额 512M
sudo python3 qemu-6.2.0/vm_swap/vm_swap.py \
    --mode swap --dirty-save --dirty-save-quota 512M
```

其它常用组合：

```bash
# 普通 fork（不使用 swap/脏页回收）
sudo python3 qemu-6.2.0/vm_swap/vm_swap.py --mode normal

# swap 模式，但不开 dirty-save
sudo python3 qemu-6.2.0/vm_swap/vm_swap.py --mode swap

# swap + dirty-save + 在 guest 中制造脏页
sudo python3 qemu-6.2.0/vm_swap/vm_swap.py --mode swap --dirty-save --dirtier

# dirty-save 不限额（省略 --dirty-save-quota 即可）
sudo python3 qemu-6.2.0/vm_swap/vm_swap.py --mode swap --dirty-save
```

### 4.1 直观对比：normal vs swap + dirty-save

两条命令都使用 `--dirtier`（每个子 VM 默认写 512 MiB，约 13 万个 4 KiB 脏页），
区别只在 pause 时是否回收脏页：

```bash
cd /home/rtzhong/qemu-playground

# A. 普通 fork：pause 不回收脏页
sudo python3 qemu-6.2.0/vm_swap/vm_swap.py --mode normal --dirtier

# B. swap + dirty-save：pause 保存脏页并 MADV_DONTNEED，used 应下降
sudo python3 qemu-6.2.0/vm_swap/vm_swap.py --mode swap --dirty-save --dirtier
```

对比每轮打印的 `reclaimed` 和最后的 `memory summary`：A 的 reclaimed 应该接近 0
（只剩测量噪声），B 的 reclaimed 应接近实际脏页总量。

本机实测（4 个子 VM，每个 `dirtier` 写 512 MiB；`MemTotal` 251.51 GiB）：

| 模式 | avg used before pause | avg used while paused | avg reclaimed on pause | avg delta after resume |
|---|---|---|---|---|
| `--mode normal --dirtier` | 26.80 GiB | 26.80 GiB | **-0.9 MiB** | 1.1 MiB |
| `--mode swap --dirty-save --dirtier` | 27.04 GiB | 24.81 GiB | **2.24 GiB** | 175.8 MiB |

`normal` 模式 pause 不回收内存，reclaimed 只有测量噪声；`swap + dirty-save`
每次 pause 稳定回收约 2.2 GiB，对应 4 个子 VM × 512 MiB 的脏页。`free` 统计的是
宿主机全局 used，包含其它进程，因此会有一定波动。

### 4.2 QMP 触发方式

`-dirty-save` 只启用能力；本次 pause 是否保存由 QMP `stop` 参数决定：

```json
// 普通 stop：不保存、不 drop
{ "execute": "stop" }

// 保存并回收，quota 使用命令行默认值
{ "execute": "stop", "arguments": { "enable-save-dirty": true } }

// 本次 pause 覆盖 quota；size 是字符串，支持 K/M/G，"0" 表示不限
{ "execute": "stop",
  "arguments": { "enable-save-dirty": true, "quota": "512M" } }
```

- `quota` 只影响本次 pause，不改变命令行默认值；不传时使用
  `-dirty-save ...,quota=` 的值。
- VM 已经是 paused 时再次 `stop` 不会保存。
- `enable-save-dirty=true` 但命令行没有 `-dirty-save` 时，`stop` 返回错误。
- 只传 `quota` 而不传 `enable-save-dirty=true` 时，`stop` 返回错误。
- harness 在 swap 模式下会自动传入 `enable-save-dirty` 和 quota。
- 快照文件固定为 `<output_dir>/dirty_save/dirty_<idx>.bin`，所有子 VM 共用
  `dirty_save` 目录但文件名不同；`--dirty-save` 开启时 harness 会预先创建该目录。

参数说明：

| 参数 | 含义 |
|---|---|
| `--mode {normal,swap}` | `normal` 用普通 `QEMUTemplate`；`swap` 用带 `/opt/qemu-exp` 路径的模板并启用 swap 相关选项 |
| `--dirtier` | fork 后在 guest 内运行 `dirtier <size>` 制造脏页 |
| `--dirtier-size N` | dirtier 分配大小，单位 MiB，默认 512 |
| `--dirty-save` | 为 fork 出的 VM 加 `-dirty-save`（仅 `--mode swap` 生效） |
| `--dirty-save-quota B` | dirty-save 配额，如 `512M`、`1G`；不传表示不限 |

注意：

- `--mode normal` 的模板启动使用 `PATH` 中的 `qemu-system-x86_64`；
  `--mode swap` 的模板和子 VM 使用 `QEMU_PATH`（默认 `/opt/qemu-exp/bin`）。
  为保证行为一致，建议把 `/opt/qemu-exp/bin` 放到 `PATH` 前面。
- 脚本中使用了 `input()`（开始 pause/resume 前、以及退出前），需要在交互式终端运行。
- 启动时会执行 `ip link del tap0` 并创建 `tap0`、`mgmt_1..mgmt_4`。正常结束会清理；
  若中途 Ctrl-C 或异常退出，手动清理：
  ```bash
  sudo ip link del tap0 2>/dev/null
  for i in 1 2 3 4; do sudo ip link del mgmt_$i 2>/dev/null; done
  ```
- 输出目录 `./vm_swap_log` 不存在时会自动创建。

每轮的宿主内存统计类似：

```text
=== round 1/4 ===
host memory before pause: 24.81 GiB used / 251.51 GiB total
pause 1
...
host memory after pause: 20.63 GiB used (reclaimed 4.18 GiB)
resume 1
...
host memory after resume: 24.80 GiB used (vs before pause -10.0 MiB)

=== memory summary (4 rounds) ===
  avg used before pause:  24.80 GiB
  avg used while paused:  20.61 GiB
  avg used after resume:  24.79 GiB
  avg reclaimed on pause: 4.19 GiB
  avg delta after resume: -10.0 MiB
```

`reclaimed` 是 pause 前后 used 的差值，`avg delta after resume` 是 resume 后相对
pause 前的净变化。

## 5. 日志与产物

默认输出目录 `./vm_swap_log`：

| 路径 | 内容 |
|---|---|
| `./vm_swap_log/hypervisor/qemu_<idx>/cmd.log` | 宿主机命令记录（命令、stdout/stderr、返回码） |
| `./vm_swap_log/hypervisor/qemu_<idx>/qemu_<idx>.log` | QEMU stdout/stderr；`-nographic -serial mon:stdio` 使 guest 串口输出也在这里；子 VM 的 `dirty-save:` 信息同样在此 |
| `./vm_swap_log/fork.log` | parallel 模式下子 VM create/start 的命令记录 |
| `./vm_swap_log/tap_manager.log` | 代码中初始化但从未 `dump_log()`，通常不会生成 |
| guest 内 `/tmp/server.log` | 模板 guest 里 `server_qemu` 的输出 |

工作目录中的其它文件：

- `qemu_<idx>/dirty_snapshot.bin`：dirty-save 在 pause 时写入，resume 成功后删除；
- `qemu_0/vm_state`、`qemu_0/vm_mem`：模板快照状态和共享内存文件；
- `qemu_<idx>/qemu_<idx>.qcow2`：子 VM 的 qcow2 覆盖层。

## 6. 常见问题

| 现象 | 排查 |
|---|---|
| `unrecognized option '-dirty-save'` | `/opt/qemu-exp/bin/qemu-system-x86_64` 不是本仓库构建的版本，重新执行 `./vm_swap/build.sh` |
| 连接 QMP socket 失败 | 对应 `qemu_<idx>.log` 查看 QEMU 是否启动；常见原因是缺 `/dev/kvm`、tap 不存在、磁盘路径不对 |
| `ip tuntap add ... Operation not permitted` | 用 root / `sudo` 运行 |
| ssh 登录失败 | 确认运行目录有 `id_rsa`，且 `base.qcow2` 内 root 的 `authorized_keys` 与之匹配 |
| `agent build did not produce ...` | 检查 `g++` 是否安装；手动执行 `make -C vm_swap/agent` 看错误 |
| 子 VM 起不来、`vm_state` 载入失败 | 确认模板阶段 `server started` 后快照成功，且 `vm_mem`/`vm_state` 未被删除 |
