# 路线图与功能清单映射

每个里程碑都必须产出可在 QEMU 中启动的镜像，并通过 `make test` 中的断言。
「预留」表示接口/数据结构就位、默认关闭，后续里程碑填充实现。

## M0 — 工程与工具链（已完成）

- 仓库分层、Makefile、ELF 链接脚本、FAT32 ESP 镜像生成。
- QEMU/OVMF 启动脚本、串口日志断言、位图字体生成器。
- 验收：`make` 产出 `build/esp.img`；`make test` 可以抓取串口日志。

## M1 — 引导与早期内核（已完成）

对应清单：一、启动与引导层；二.1 的 GDT/IDT 基础；六.3 工具链。

- UEFI 应用引导、ELF64 装载、GOP/RSDP/内存图采集。
- 64 GiB 身份映射、CR3 切换、Boot Info Block 交接。
- 串口 + 帧缓冲控制台、GDT/TSS、256 向量 IDT、异常与 panic。
- 验收：QEMU 串口出现 `MCUBE_BOOT_OK`；屏幕显示 banner；
  故意触发 `#PF` 时打印 CR2、错误码与栈回溯并 halt。

## M2 — 物理/虚拟内存（已完成）

对应清单：二.2、二.6 的一部分。

- 物理页帧分配器（位图），预留内核/loader/MMIO/ACPI。
- 四级页表管理：direct map、vmalloc；内核已迁移到 higher-half。
- 内核堆：slab 对象缓存 + 大对象页运行，`kmalloc/kfree`，泄漏统计。
- 每进程地址空间：低半区独立 PML4，高半区共享内核映射。
- NX 已启用；COW、SMAP/SMEP 与完整 W^X 加固留到 M10。
- 验收：`mm_selftest` 通过（4096 页分配/释放无泄漏、混合堆对象、
  大对象与 `krealloc`）。

## M3 — 中断、时钟与 SMP（部分完成）

对应清单：二.1、二.5、二.4（自旋锁）。

- 已实现：PIT 1000 Hz、RTC、软定时器、PIC、自旋锁/原子操作、
  抢占式 RR 调度、睡眠/唤醒。
- 待实现：LAPIC/IOAPIC、APIC timer、HPET/TSC、softirq/workqueue、
  ACPI MADT 与 AP 启动。
- 验收：当前以 PIT + 单核 RR 通过；双核与锁反转检测随 SMP 里程碑补齐。

## M4 — 进程、调度与系统调用（核心完成）

对应清单：二.3、二.6、三.6、四.1 的 syscall 包装。

- 已实现：`syscall`/`sysret`（MSR STAR/LSTAR/FMASK、EFER.SCE）、
  swapgs + 每 CPU 内核栈、TSS.rsp0、独立用户地址空间、用户态 ELF
  装载器、PID 1 init、`write/getpid/nanosleep/yield/exit`、负 errno、
  `copy_from_user/copy_to_user` 边界检查。
- 待实现：`fork/execve/wait/kill`、线程 create/join、FPU 惰性切换、
  MLFQ。这些依赖 M5 的 VFS/exec 路径，随后续里程碑补齐。
- 验收：init 在用户态打印并退出；非法指针返回 `-EFAULT`、超大长度
  返回 `-EINVAL`，内核不 panic。

## M5 — 文件系统、块设备、TTY（核心完成）

对应清单：三.1–三.3、三.5 的一部分。

- 已实现：
  - 块设备注册层 + ATA PIO 驱动（主通道，LBA28 读写/IDENTIFY）。
  - FAT32 读写：superfloppy 与 MBR 首分区、8.3 目录项、簇链分配/释放、
    创建/删除/截断、目录扩展、双 FAT 同步与扇区缓存。
  - VFS：最长前缀挂载、`vfs_inode`/`vfs_file` 抽象、每任务 16 项 fd 表。
  - 挂载：FAT32 `/`、tmpfs `/tmp`、devfs `/dev`
    （`console`、`tty0`、`ttyS0`、`null`、`zero`）。
  - 系统调用：`open/read/write/close/stat/fstat/getdents64/mkdir/unlink`。
  - TTY 输入：PS/2 键盘 IRQ1 + COM1 RX IRQ4 汇入环形队列；
    非阻塞读取（空队列返回 `-EAGAIN`）。
- 待实现：AHCI/NVMe、块层请求队列与错误重试、页缓存、LFN 长文件名、
  PTY/行规程/前后台进程组、鼠标与 `/dev/input/*`。
- 验收：内核自检列根目录、读 `/kernel.elf` 的 ELF magic、FAT32 与 tmpfs
  写入回读、`/dev/zero`；init 读取 `/TEST.TXT`、枚举 `/`、`stat`
  内核 ELF，并经 `/dev/ttyS0` 输出。`make test` 断言全部通过。

## M6 — 网络（核心完成）

对应清单：三.4。

- 已实现：
  - PCI legacy 配置空间枚举、BAR 解码、bus-master 使能。
  - Intel 8254x（e1000，QEMU 82540EM）轮询驱动：MMIO BAR、复位、
    RX/TX 描述符环、MAC 读取、link 状态；IMS=0，不依赖 PCI IRQ 路由。
  - 协议栈：Ethernet/ARP/IPv4/ICMP echo/UDP/DNS（A 记录解析）。
  - BSD socket：`socket/bind/connect/sendto/recvfrom/send/recv/close`；
    socket 与文件共用 fd 空间（`task_t->socks[]`），任务退出自动回收。
  - 系统调用：`socket(41)/connect(42)/sendto(44)/recvfrom(45)/bind(49)`，
    `sockaddr_in` 使用 Linux 线格式；TCP 返回 `-ENOSYS`。
- 待实现：TCP（握手/重传/窗口）、DHCP、IPv6、路由表、多网卡、
  RX 中断与 NAPI 式批量回收、`setsockopt/getsockopt`。
- 验收：QEMU user 网络下 ARP 到 `10.0.2.2`、ICMP echo、DNS 解析
  `example.com`；init 在用户态用 UDP socket 向 `10.0.2.3:53` 查询并
  收到应答。`make test` 断言 `NET_PING_OK`/`NET_DNS_OK`/`NET_TEST_OK`
  与 `USER_NET_OK` 全部通过。

## M7 — 用户空间基础（libc + Shell + coreutils）（已完成）

对应清单：四.1–四.3。

- 已实现：
  - 用户态 libc（`user/lib`）：`crt0` 从内核构造的 argc/argv/envp 启动、
    syscall 包装（负 errno 转 `errno`/`-1`）、字符串、`printf/snprintf/
    fprintf`、`malloc/calloc/realloc`（`brk` 上的 bump 分配器）、
    `getenv/setenv/unsetenv`、`environ`。
  - 进程控制：`fork`（完整寄存器恢复 + 地址空间克隆 + fd 表按引用继承）、
    `execve/execv/execvp`（ELF 装载、argv/envp 栈、保留 fd）、
    `wait4`（僵尸回收与退出状态）、`pipe`、`dup/dup2`（引用计数复制）、
    `chdir/getcwd/brk/getppid/rmdir`，相对路径按任务 cwd 解析。
  - 管道：4 KiB 环形缓冲、阻塞读写、读端/写端独立引用计数，
    `SIGPIPE` 语义简化为 `-EPIPE`。
  - Shell（`user/programs/sh.c`）：分词、单/双引号与转义、`$VAR/${VAR}`/
    `$?`/`$$` 展开、`;` 顺序执行、`|` 多级管道、`>`/`>>`/`<` 重定向、
    `&` 后台作业、内建 `cd/pwd/exit/export/unset/set/jobs/help/echo/
    true/false`，命令经 `/bin/<name>.elf` 解析。
  - coreutils（20 个）：`sh ls cat echo mkdir rm cp mv pwd sleep wc grep
    head tail uname true false clear env help`，构建后以 8.3 大写名放入
    ESP 的 `/BIN`（如 `SH.ELF`）。
- 待实现：pthread 轻量线程、历史记录与 Tab 补全、`Ctrl+Z`/`bg`/`fg`
  完整作业控制（当前仅后台作业）、`make libc-test` host 单元测试。
- 验收：PID 1 完成全部既有自检后 fork/exec `/bin/sh.elf -c
  "ls /bin | wc -l"`，管道输出行数；`echo ... > /tmp/m7.txt` 重定向
  回读一致；交互式 Shell 可执行 `help`、`uname -a`、
  `echo hello-pipe | wc -c`。`make test` 断言 `USER_SHELL_OK` 与
  `USER_REDIR_OK`。

## M8 — 图形子系统与桌面

对应清单：四.4–四.6。

- GOP 双缓冲、图形基元、位图/TTF 子集字体（中英文）、事件队列。
- 窗口管理器、合成器、libui 控件、任务栏/开始菜单/登录界面。
- 文件管理器、终端、编辑器、计算器、图片查看器、设置、任务管理器。
- 验收：QEMU 截图中可见桌面；窗口可拖动/缩放/最小化；
  终端内运行 Shell；文件管理器可以浏览 FAT32 分区。

## M9 — AI 原生层

对应清单：五、六.1–六.2。

- `aiosd`：GGUF(Q4/Q5/Q8) 加载、CPU 推理（SIMD）、BPE、采样、流式输出。
- 上下文管理器（截断/摘要/KV cache 快照恢复）。
- 记忆管理器（LRU-K 短记忆）+ 存储管理器（会话归档、本地向量检索）。
- Agent 调度器、工具管理器（文件/进程/网络/计算/沙箱执行）、Access Manager。
- `ai_*` 系统调用号段、事件通知、`ai` CLI、Shell `?` 集成、桌面 AI 客户端。
- 验收：离线完成单轮与多轮对话；`ai exec "查看内存占用最高的进程"` 正确调用工具；
  未授权目录访问被拒绝且写入审计日志；重启后恢复最近会话。

## M10 — 安全加固、可观测性与发布

对应清单：六。

- 系统调用模糊测试、输入长度限制、SMAP/SMEP 回归、沙箱 seccomp 风格白名单。
- dmesg 环形缓冲、统计计数、核心转储、故障自检（AI 生成中文诊断）。
- 单元测试（用户态模拟内核子系统）、CI、发布镜像与文档。
- 验收：fuzz 24h 无 panic；`dmesg` 可导出；AI 故障说明可读且引用真实日志。
