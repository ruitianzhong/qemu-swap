# QEMU 6.2.0 构建与安装指南

本文件总结本项目中 QEMU 6.2.0 的构建方式，以及如何指定安装目录。
文件位于 `vm_swap/` 目录；文中 `./vm_swap/install.sh`、`../configure` 等命令均以
**仓库根目录**为基准（即 `vm_swap/` 的上一级）。

## 1. 构建系统概览

QEMU 6.2.0 采用 **out-of-tree（源码树外）构建**，构建流程分两步：

1. **`configure`**：自动探测宿主机环境（编译器、依赖库等），生成 meson 构建描述（`build.ninja`）及配置文件（`config-host.mak`、`config-host.h` 等）。
2. **`make`**（底层调用 meson/ninja）：真正编译链接，产出各架构的 `qemu-system-*`、`qemu-linux-*`、`qemu-img`、`qemu-nbd` 等二进制。

### 1.1 为什么使用源码树外构建

- 源码目录始终保持干净，`git status` 不会被生成物污染。
- 可以用同一个源码树配置多套不同的构建（不同目标架构、不同编译选项），互不干扰。
- 官方推荐流程（见 `README.rst` 的 Building 章节）：

```bash
mkdir build
cd build
../configure
make
```

### 1.2 当前项目构建状态

本项目已经配置并构建过，位于仓库顶层 `build/` 目录。其 meson 配置记录在
`build/meson-private/cmd_line.txt` 中：

| 配置项 | 当前值 |
|--------|--------|
| `prefix`     | `/opt/qemu-exp` |
| `bindir`     | `/opt/qemu-exp/bin` |
| `libdir`     | `/opt/qemu-exp/lib` |
| `datadir`    | `/opt/qemu-exp/share` |
| `sysconfdir` | `/opt/qemu-exp/etc` |

即当前构建安装到 **`/opt/qemu-exp`**，与 `vm_swap/vm_swap.py` 中的
`QEMU_PATH` 一致。

### 1.3 一键构建脚本 `vm_swap/build.sh`

`vm_swap/build.sh` 封装了 configure（需要时）+ make + make install：

```bash
# 在仓库根目录
./vm_swap/install.sh           # 首次构建时安装依赖
./vm_swap/build.sh             # 构建并安装到 /opt/qemu-exp
```

常用选项：

```bash
./vm_swap/build.sh --clean       # 删除 build/ 后从头构建
./vm_swap/build.sh --no-install  # 只编译，不执行 make install
./vm_swap/build.sh --help        # 查看完整用法
```

可通过环境变量覆盖默认值：`PREFIX`、`TARGETS`、`JOBS`，例如：

```bash
PREFIX=/opt/qemu-exp TARGETS=x86_64-softmmu JOBS=16 ./vm_swap/build.sh
```

脚本内部会自动定位仓库根目录（`build.sh` 的上一级），因此也可以先
`cd vm_swap` 再执行 `./build.sh`。

## 2. 依赖安装

Linux 下编译 QEMU 依赖的打包工具（meson 构建需要 glib2/pixman/fdt/meson 等），
本项目 `vm_swap/install.sh` 中已经列出，关键项为：

```bash
sudo apt install libglib2.0-dev libpixman-1-dev libfdt-dev flex bison
sudo apt install meson ninja-build python3-pip
```

## 3. 如何指定安装目录

安装目录的指定全部发生在 **`configure` 阶段**，通过 `--prefix` 及相关的子目录参数。

### 3.1 最常用：`--prefix=PREFIX`

`--prefix` 是所有其余安装路径的基准。默认值为 `/usr/local`。

```bash
cd build
../configure --prefix=/opt/qemu
```

不指定具体子目录时，安装文件会散布到：

| 目录项 | 默认位置（相对 `prefix`） |
|--------|--------------------------|
| 可执行文件 | `$prefix/bin` |
| 库文件 | `$prefix/lib` |
| 包含头文件 | `$prefix/include` |
| 固件/数据 | `$prefix/share/qemu` |
| 文档 | `$prefix/share/doc/qemu` |
| 手册页 | `$prefix/share/man` |
| 配置文件 | `$prefix/etc/qemu` |
| 状态数据 | `$prefix/var` |

### 3.2 独立覆盖单个子目录

`configure --help` 中列出了可单独覆盖的安装路径参数（高级选项）：

```bash
--prefix=PREFIX        # 安装基准目录（默认 /usr/local）
--bindir=PATH          # 可执行文件目录
--libexecdir=PATH      # 辅助程序目录
--libdir=PATH          # 库文件目录
--includedir=PATH      # 头文件目录
--datadir=PATH         # 固件数据（实际在 PATH/qemu 下）
--localedir=PATH       # 翻译文件
--docdir=PATH          # 文档
--sysconfdir=PATH      # 配置文件
--localstatedir=PATH   # 本地状态
--mandir=PATH          # 手册页
--with-suffix=SUFFIX   # 数据在 datadir/libdir/sysconfdir/docdir 下的后缀（默认 qemu）
```

> 注：`--bindir` 在同一语义下覆盖 `prefix/bin`；`--datadir`、`--libdir` 等在
> 后续还会追加 `/$qemu_suffix`（默认 `qemu`，可用 `--with-suffix` 修改或置空）。

### 3.3 DESTDIR：打包/临时安装（不影响 configure 结果）

`make install` 支持标准 `DESTDIR` 变量，用于把安装内容重定向到临时目录，
常用于打包（RPM/deb）或安装到非 root 目录后再搬迁，**无需重新 configure**：

```bash
cd build
make DESTDIR=/tmp/qemu-staging install
```

此时实际安装到 `/tmp/qemu-staging/usr/local/...`（`DESTDIR` + `prefix` 拼接）。

> 提示：以非 root 用户安装到系统目录（如 `/usr/local`）通常需要 `sudo make install`；
> 若想装到个人目录，改为 `../configure --prefix=$HOME/qemu` 更省事。

## 4. 安装完成后的验证

```bash
build/x86_64-softmmu/qemu-system-x86_64 --version   # 运行本构建的模拟器
```

如果在 `build` 之外安装了系统目录，用安装后的位置：

```bash
/opt/qemu/bin/qemu-system-x86_64 --version
which qemu-system-x86_64                             # 确认 PATH 命中
```

## 5. 常见构建命令示例

```bash
# 1) 全新构建，安装到 /opt/qemu
mkdir build && cd build
../configure --prefix=/opt/qemu
make -j$(nproc)
sudo make install

# 2) 只构建部分目标架构以加快速度
../configure --prefix=/opt/qemu \
             --target-list=x86_64-softmmu,aarch64-softmmu
make -j$(nproc)

# 3) 用 DESTDIR 打包安装
../configure --prefix=/usr/local
make -j$(nproc)
make DESTDIR=$PWD/../staging install
```

## 6. x86 与 ARM 相关编译选项

x86 / ARM 是 QEMU 最常用的宿主与目标架构。configure 对它们的支持分两层：
**目标架构选择**（决定编译哪些 `qemu-system-*`）和**宿主/平台特定选项**（针对这台
构建机及平台特性）。

### 6.1 目标架构选择（`--target-list`）

用 `--target-list` 选择要编译的架构。每个条目为 `架构-模式`；一个架构通常既有
`-softmmu`（整机模拟）也有 `-linux-user`（用户态模拟）。

**x86 / x86_64：**
```bash
--target-list=x86_64-softmmu,i386-softmmu          # 整机模拟
--target-list=x86_64-linux-user,i386-linux-user    # 用户态模拟
--target-list=x86_64-softmmu,i386-softmmu,x86_64-linux-user
```

**ARM / AArch64：**
```bash
--target-list=aarch64-softmmu,arm-softmmu                      # 整机模拟
--target-list=aarch64-linux-user,arm-linux-user,armeb-linux-user
--target-list=aarch64-softmmu,arm-softmmu,aarch64-linux-user,arm-linux-user
```

只各取一个典型目标做演示（推荐最小组合）：
```bash
../configure --prefix=/opt/qemu \
    --target-list=x86_64-softmmu,aarch64-softmmu
```

> 补充：configure 是无法通过命令行细分"某块 ARM 开发板"的，具体板级设备
> 由 `configs/devices/`（Kconfig）控制，命令行层面只到"架构"这一粒度。
> 运行时的板级选择发生在 QEMU 启动参数 `-machine`（如 `-machine virt`、
> `-M raspi3b`）中，与编译选项无关。

### 6.2 x86 平台特定选项

| 选项 | 功能 |
|------|------|
| `--cpu=CPU` | 指定**宿主** CPU 类型（自动检测，一般无需设置），如 `--cpu=x86_64`、`--cpu=i386`。 |
| `--enable-avx2` | 启用 AVX2 优化（加速某些运算路径） |
| `--disable-avx2` | 关闭 AVX2 优化 |
| `--enable-avx512f` | 启用 AVX512F 优化（默认关闭，需编译器支持） |
| `--disable-avx512f` | 关闭 AVX512F 优化 |

示例：开启 x86 指令集加速，并只编 x86 整机模拟：
```bash
../configure --prefix=/opt/qemu \
    --target-list=x86_64-softmmu \
    --cpu=x86_64 --enable-avx2 --enable-avx512f
```

> 说明：AVX 相关选项是**宿主 CPU 指令集优化**（影响 QEMU 自身运行速度），
> 与目标架构无关，任何架构都能配合使用；只是通常在与 x86 宿主搭配时提及。

### 6.3 ARM 平台特定选项

| 选项 | 功能 |
|------|------|
| `--efi-aarch64=PATH` | 指定用于 aarch64 VM 的 EFI 固件文件路径（构建/打包时嵌入） |
| `--cpu=CPU` | 指定宿主 CPU（ARM 宿主自动检测为 `arm`/`aarch64`，一般无需设置） |

ARM 没有像 x86 AVX 那样独立的指令集加速开关；ARM 相关选项较少，核心仍是
目标选择 + 事后用 `-machine` 选择板级。示例：
```bash
../configure --prefix=/opt/qemu \
    --target-list=aarch64-softmmu,arm-softmmu \
    --efi-aarch64=/usr/share/qemu-efi-aarch64/QEMU_EFI.fd
```

### 6.4 两个平台都常用（但非专属）的选项

虽然非 x86/ARM 专属，但这两个平台最常配合使用：

| 选项 | 用途 |
|------|------|
| `--enable-kvm` | 在本机用 KVM 加速（x86_64/aarch64 宿主很常见） |
| `--enable-tcg` / `--enable-tcg-interpreter` | 软件模拟后端，无 KVM 时兜底 |
| `--enable-gtk / --enable-sdl / --enable-vnc --enable-curses` | 图形/远程/字符界面 |
| `--enable-tools` | 构建 qemu-img / qemu-nbd / qemu-io |
| `--enable-seccomp` | 安全沙箱 |

这些通用特性开关的完整清单见 `COMPILE_OPTIONS.md`。

### 6.5 KVM 加速选项专题

KVM 是 x86_64 / aarch64 宿主最常用的硬件加速，相关编译选项定义于
`meson_options.txt`（`type: feature, value: 'auto'`）：

| 选项 | 行为 |
|------|------|
| `--enable-kvm` | 强制启用 KVM；宿主缺 KVM 头文件 / 平台不支持时报错退出 |
| `--disable-kvm` | 强制关闭 KVM |
| （不指定）默认 `auto` | 自动检测，宿主支持 KVM 就编译 |

```bash
# 显式要求 KVM（推荐，缺依赖会立刻暴露）
../configure --target-list=x86_64-softmmu --enable-kvm

# 或完全不要 KVM
../configure --target-list=x86_64-softmmu --disable-kvm
```

> 关键区别：**编译期** `--enable-kvm` 只决定是否编译 KVM 支持代码；
> **运行期**是否真正使用 KVM 由 QEMU 启动参数决定（`-accel kvm` /
> `-enable-kvm`）。即便编译了 KVM，跑时若 `/dev/kvm` 不可用，QEMU 会自动退回
> TCG（软件模拟）。

同类的加速器选项（均为 `auto` 默认）：

| 选项 | 对应加速器/后端 |
|------|----------------|
| `--enable-hax` | HAXM（Intel 硬件加速） |
| `--enable-hvf` | Hypervisor.framework（macOS） |
| `--enable-whpx` | Windows Hypervisor Platform |
| `--enable-nvmm` | NetBSD NVMM |
| `--enable-xen` | Xen 后端 |
| `--enable-tcg` | 软件模拟（JIT，兜底用，通常默认开启） |

## 7. 关键结论（速查）

- **构建**：`mkdir build && cd build && ../configure && make`（out-of-tree，meson 驱动）。
- **指定安装目录**：`.--configure 阶段` 用 `--prefix=/custom/path`；可再用
  `--bindir/--libdir/--datadir` 等细化。
- **默认安装目录**：`/usr/local`。
- **打包/临时重定向**：`make DESTDIR=/some/dir install`（无需重跑 configure）。
- **当前项目现状**：`build/` 已配置，`prefix=/usr/local`（默认）。