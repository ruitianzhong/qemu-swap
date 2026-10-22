# QEMU 6.2.0 compile 编译选项详解

QEMU 的 `configure` 脚本提供了大量编译选项来开启/关闭各类特性（加速器、
块设备驱动、网络后端、UI、加密库、调试功能等）。本文列出常用编译选项并说明
如何通过 `configure` 查看它们。

## 1. 如何查看 configure 选项

QEMU 采用 **out-of-tree 构建**，`configure` 须在**空的构建目录**中运行。
因此要在配置前实时查看全部选项，请在临时目录里运行：

### 1.1 查看全部选项

```bash
mkdir -p /tmp/qh && cd /tmp/qh
/path/to/qemu/configure --help     # 用项目 configure 的绝对路径
```

> 注意：**不能在已有内容（或已被 configure 占用）的目录里跑 `--help`**。
> QEMU 6.2.0 的 configure 会把构建目录当参数，若该目录已存在且非它创建的，
> 会直接报错 `ERROR: <dir> dir already exists`，不会打印选项。
> 因此使用一个空目录（或先配置后的 `build/` 目录内运行）即可。

### 1.2 只筛选特性类选项（--enable / --disable）

```bash
./configure --help 2>/dev/null | sed -n '/Optional features/,$p'
```

### 1.3 配置后回看最终生效的选项

configure 成功结束后会打印一份 **summary**（列出启用的加速器、UI、
体积大的库等）。若要看某个目录的具体编译开关，也可检查生成的
`config-host.mak`：

```bash
grep '^CONFIG_\|^.*=' build/config-host.mak | sort
```

## 2. 选项的通用规则

- 每个特性用 `--enable-FEATURE` 开启、`--disable-FEATURE` 关闭
  （例如 `--enable-kvm` / `--disable-kvm`）。
- 默认策略：**能找到依赖库就自动启用**；手动 `--enable-FEATURE` 时，
  若依赖不满足会在 configure 阶段直接报错退出（而不是静默降级）。
- 部分特性用 `--enable-FEATURE=CHOICE` 指定获取方式，例如：
  ```bash
  --enable-fdt=auto        # 自动查找系统库，找不到则用内置
  --enable-fdt=internal    # 强制使用源码内置
  --enable-fdt=system      # 强制使用系统库
  --enable-fdt=disabled    # 关闭
  ```
- 想快速关闭全部可选特性，用 `--without-default-features`。

## 3. 分类列出可选编译特性

### 3.1 加速器 / 虚拟化后端（Acceleration）

| 选项 | 功能 |
|------|------|
| `--enable-kvm` | KVM 加速（Linux） |
| `--enable-hax` | HAXM 加速（Intel 硬件加速，跨平台） |
| `--enable-hvf` | Hypervisor.framework 加速（macOS） |
| `--enable-nvmm` | NetBSD NVMM 加速 |
| `--enable-whpx` | Windows Hypervisor Platform 加速 |
| `--enable-tcg` | TCG 软件模拟（JIT，默认） |
| `--enable-tcg-interpreter` | TCG 字节码解释器（慢速，替代 JIT） |
| `--enable-xen` / `--enable-xen-pci-passthrough` | Xen 后端 / Xen PCI 直通 |

### 3.2 块设备驱动 / 镜像格式（Block device drivers）

| 选项 | 功能 |
|------|------|
| `--enable-rbd` | Ceph RADOS Block Device |
| `--enable-curl` | CURL 远程块设备 |
| `--enable-libiscsi` | iSCSI 用户态发起端 |
| `--enable-libnfs` | libnfs NFS 块设备 |
| `--enable-libssh` | SSH 块设备 |
| `--enable-glusterfs` | GlusterFS 块设备 |
| `--enable-libxml2` | Parallels 镜像格式所需 XML |
| `--enable-fuse` / `--enable-fuse-lseek` | FUSE 块导出（含 SEEK_HOLE/DATA） |
| `--enable-bzip2` | DMG 镜像解压（bzip2） |
| `--enable-lzfse` | DMG 镜像解压（lzfse） |
| `--enable-lzo` / `--enable-snappy` / `--enable-zstd` | 压缩支持 |
| `--enable-bochs --enable-cloop --enable-dmg` | 旧镜像格式支持 |
| `--enable-qcow1 --enable-vdi --enable-vvfat --enable-qed` | 旧镜像格式支持 |
| `--enable-parallels` | Parallels 镜像格式 |
| `--enable-libpmem` | 持久内存（PMEM）支持 |
| `--enable-libdaxctl` | libdaxctl（管理 PMEM 命名空间） |

### 3.3 网络后端（Network backends）

| 选项 | 功能 |
|------|------|
| `--enable-slirp[=CHOICE]` | 用户态网络栈（slirp） |
| `--enable-l2tpv3` | l2tpv3 网络后端 |
| `--enable-netmap` | netmap 网络后端 |
| `--enable-vde` | VDE 网络后端 |
| `--enable-smbd` 相关（`--slirp-smbd`） | slirp 中的 SMB 文件共享 |

### 3.4 用户界面 / 显示（UI / Display）

| 选项 | 功能 |
|------|------|
| `--enable-vnc` | VNC 服务器（远程图形） |
| `--enable-vnc-jpeg --enable-vnc-png --enable-vnc-sasl` | VNC 的 JPEG/PNG/SASL 支持 |
| `--enable-gtk`（配 `--enable-vte`） | GTK+ 界面（vte 为终端） |
| `--enable-sdl`（配 `--enable-sdl-image`） | SDL 界面 |
| `--enable-curses` | curses 字符界面 |
| `--enable-cocoa` | Cocoa 界面（仅 macOS） |
| `--enable-opengl` / `--enable-virglrenderer` | OpenGL / virgl 渲染 |
| `--enable-xkbcommon` | xkbcommon 键盘布局 |
| `--enable-gettext` | GTK 界面的本地化 |
| 音频相关 | `--enable-alsa --enable-pa --enable-oss --enable-jack --enable-coreaudio --enable-dsound` |

### 3.5 加密 / 安全 / 认证（Crypto & security）

| 选项 | 功能 |
|------|------|
| `--enable-gnutls` | GNUTLS 加密 |
| `--enable-gcrypt` / `--enable-nettle` | libgcrypt / nettle 加密后端 |
| `--enable-crypto-afalg` | Linux AF_ALG 加密后端 |
| `--enable-seccomp` | seccomp 沙箱 |
| `--enable-selinux` | qemu-nbd 的 SELinux 支持 |
| `--enable-auth-pam` | PAM 访问控制 |
| `--enable-cap-ng` | libcap-ng |

### 3.6 设备 / 外设（Devices）

| 选项 | 功能 |
|------|------|
| `--enable-tpm` | TPM 设备 |
| `--enable-smartcard` | CA 智能卡模拟 |
| `--enable-u2f` | U2F 安全密钥模拟 |
| `--enable-usb-redir` | USB 重定向 |
| `--enable-libusb` | USB 直通（passthrough） |
| `--enable-libudev` | 用 libudev 枚举主机设备 |
| `--enable-multiprocess` | 进程外设备模拟 |
| `--enable-virtfs` | virtio-9p 文件系统 |
| `--enable-virtiofsd` | 构建 virtiofs 守护进程 |
| `--enable-vhost-user-blk-server` | vhost-user-blk 服务器 |

### 3.7 vhost / virtio 加速（Kernel virtio backend）

| 选项 | 功能 |
|------|------|
| `--enable-vhost-net` | vhost-net 内核加速 |
| `--enable-vhost-vsock` | virtio socket 设备 |
| `--enable-vhost-scsi` | vhost-scsi 内核 target |
| `--enable-vhost-crypto` | vhost-user-crypto |
| `--enable-vhost-kernel` | vhost 内核后端 |
| `--enable-vhost-user` | vhost-user 后端 |
| `--enable-vhost-vdpa` | vhost-vdpa 内核后端 |
| `--enable-vhost-net`、`--enable-vhost-vsock` 等 | 同上组合 |

### 3.8 迁移 / 网络 / 运算优化（Migration & optimizations）

| 选项 | 功能 |
|------|------|
| `--enable-rdma` | RDMA 迁移 |
| `--enable-pvrdma` | PVRDMA 设备 |
| `--enable-live-block-migration` | 主线迁移流中的块迁移 |
| `--enable-avx2` / `--enable-avx512f` | AVX2 / AVX512F 优化 |
| `--enable-bpf` | eBPF 支持（QMP/防火墙 filter） |
| `--enable-membarrier` | membarrier 系统调用 |
| `--enable-coroutine-pool` | coroutine 空闲池（提高性能） |

### 3.9 调试 / 开发（Developer & debugging）

| 选项 | 功能 |
|------|------|
| `--enable-debug` | 常用 debug 构建选项 |
| `--enable-debug-info` | 生成调试信息 |
| `--enable-debug-tcg` | TCG 调试 |
| `--enable-debug-stack-usage` | 栈使用量跟踪 |
| `--enable-debug-mutex` | 互斥锁调试 |
| `--enable-qom-cast-debug` | QOM 类型转换调试 |
| `--enable-sanitizers` | 默认 sanitizer（ASan 等） |
| `--enable-tsan` | ThreadSanitizer |
| `--enable-gcov` | gcov 覆盖率分析 |
| `--enable-gprof` | gprof 性能分析 |
| `--enable-profiler` | profiler 支持 |
| `--enable-cfi` / `--enable-cfi-debug` | 控制流完整性（CFI）保护 |
| `--enable-lto` | 链接期优化 LTO |
| `--enable-static` | 静态链接 |
| `--enable-werror` | 编译警告视为错误 |
| `--enable-modules` / `--enable-module-upgrades` | 模块化加载 |
| `--enable-plugins` | TCG 插件（共享库加载） |

### 3.10 工具 / 其他（Tools & misc）

| 选项 | 功能 |
|------|------|
| `--enable-tools` | 构建 qemu-img/qemu-io/qemu-nbd 等工具 |
| `--enable-guest-agent` | 构建 QEMU Guest Agent |
| `--enable-guest-agent-msi` | Guest Agent 的 MSI 安装包 |
| `--enable-fuzzing` | 构建 fuzz 测试目标 |
| `--enable-trace-backends=CHOICE` | 选择 trace 后端（log/simple/ust/syslog/ftrace/nop/dtrace） |
| `--enable-docs` | 构建文档 |
| `--enable-sparse` | sparse 静态检查 |
| `--enable-gio` | libgio（glib 的 GIO） |
| `--enable-rng-none` | 使用假 RNG（不读 /dev/urandom） |
| `--enable-socket`... | （无此项） |
| `--enable-slirp-smbd` | slirp 网络中使用 smbd |
| 体系相关 | `--enable-system --enable-user --enable-linux-user --enable-bsd-user` 控制模拟器类别 |

### 3.11 安装 / 打包相关（非特性，但同属 configure 选项）

| 选项 | 功能 |
|------|------|
| `--prefix=PREFIX` | 安装基准目录（默认 `/usr/local`） |
| `--bindir/--libdir/--datadir/--mandir/--systemconfdir` | 覆盖各子目录 |
| `--disable-install-blobs` | 不安装固件 blobs |
| `--with-pkgversion=VER` | 自定义包版本串 |

## 4. 常用编译示例

```bash
mkdir build && cd build

# 最小化安装 + 常用加速/UI，安装到 /opt/qemu
../configure --prefix=/opt/qemu \
    --target-list=x86_64-softmmu,aarch64-softmmu \
    --enable-kvm --enable-tcg \
    --enable-sdl --enable-vnc --enable-gtk \
    --enable-curses

# 开发调试构建
../configure --enable-debug --enable-sanitizers --enable-werror

# 静态全功能构建
../configure --static --enable-tools --enable-guest-agent
```

## 5. 关键结论（速查）

- **查看全部选项**：`cd <空目录> && <qemu>/configure --help`
  （configure 必须在空构建目录里运行，`--help` 同理）。
- **特性开关格式**：`--enable-FEATURE` / `--disable-FEATURE`，
  默认若有依赖库则自动启用。
- **强制启用**：`--enable-FEATURE` 在依赖缺失时会报错退出。
- **按需强制关闭全部**：`--without-default-features`。
- **部分特性可选来源**：`--enable-fdt=/slirp/capstone=auto|internal|system`。
- **验证生效项**：configure 结束的 summary，或 `grep CONFIG_ build/config-host.mak`。