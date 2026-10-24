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

## M8 — 图形子系统与桌面（已完成）

对应清单：四.4–四.6。

- 已实现：
  - 帧缓冲：`SYS_FB_INFO` 读取 GOP 模式（1280×800 BGRA，pitch 5120），
    `SYS_FB_MAP` 把物理帧缓冲映射到用户 VA `0x3000_0000`（`PTE_PCD`，
    软件位 `PTE_SHARED` 防止 fork/退出误释放 MMIO），随后关闭内核
    帧缓冲控制台，日志只走串口。
  - 输入：PS/2 键盘 IRQ1 与鼠标 IRQ12 汇入 256 项环形事件队列，
    `SYS_INPUT_POLL` 读取；方向键/Delete/Home/End/PgUp/PgDn/F1–F12
    使用 `0x100` 以上的键码，鼠标 3 字节包解析并钳制到屏幕范围。
  - 桌面协议（`include/mcube/gui.h`）：应用不接触 framebuffer。每个窗口
    由桌面创建两条管道并作为 fd 3（命令）/fd 4（事件）交给应用；
    `gui_cmd_t`（INIT/CLEAR/FILL/RECT/TEXT/FLUSH/TITLE/CLOSE/LAUNCH/KILL）
    与 `gui_event_t`（RESIZE/KEY/MOUSE/FOCUS/CLOSE/TICK）均为定长结构。
  - 合成器（`user/programs/desktop.c`）：独占 framebuffer，为每个窗口
    维护离屏 surface，负责裁剪、Z 序、标题栏/关闭按钮、平铺布局、
    任务栏、开始菜单、时钟与鼠标光标保存/恢复；输入按焦点路由。
  - `SYS_SPAWN`：内核把 ELF 装入全新地址空间并把父进程的两个 fd 安装为
    子进程 fd 3/4，桌面因此不必 fork 自己的大堆；`SYS_POLL` 提供
    pipe/socket/tty/普通文件的可读等待，`SYS_TIME`/`SYS_TASKINFO`
    供任务栏与任务管理器使用，`SYS_KILL` 由桌面代应用终止进程。
  - 应用：Terminal（fork `/bin/sh.elf` + 双管道、字符网格、按窗口宽度
    重排）、Files（FAT32 目录浏览、大小显示、启动编辑器）、Editor
    （16 KiB 缓冲、光标/滚动、F2 保存）、Calculator（整数表达式解析、
    鼠标/键盘）、Image Viewer（程序化风景、双调色板）、Settings
    （帧缓冲/时间/任务信息、主题、快捷启动）、Task Manager（进程列表、
    选择、End Task）。
  - PID 1 完成既有自检后 fork/exec `/bin/desktop.elf`，父进程继续
    `execv("/bin/sh.elf")` 保留串口控制台。
- 待实现：窗口拖动/缩放/最小化、登录界面、中文位图/TTF 字体子集、
  剪贴板、libui 控件库、应用间共享内存与拖放。
- 验收：`make test` 断言 `USER_DESKTOP_OK` 与 `USER_GUI_OK`（7 个窗口
  全部 INIT+FLUSH）；`make screenshot` 截图可见桌面、任务栏、菜单与
  7 个应用；`make gui-test` 通过 QEMU monitor 发送 F1/方向键/回车与
  PS/2 鼠标点击，验证菜单导航、应用启动与焦点切换。

## M9 — AI 原生层（基线完成）

对应清单：五、六.1–六.2。

- 已实现（基线）：
  - 内核侧：`SYS_AI_CONNECT/ACCEPT/STATUS/AUDIT/LOG`（`0x400` 段），
    每个会话由两条匿名管道组成（客户端 fd 3/4，守护进程对端），
    守护进程注册与单实例检查，64 项内核审计环（仅守护进程可写）。
  - 传输：528 字节定长 `ai_msg_t`（type/session/flags/len/data），
    大消息分片；用户态 `ai_send/ai_recv` 按整条消息收发，避免内核
    512 字节管道读上限造成截断。
  - 推理：GGUF v3 解析（llama 元数据、F32 张量）、字节级 256 词表、
    llama 风格单层前向（RMSNorm/注意力/FFN，可选）、top-k + 温度采样、
    SSE2 数学内核；内置 `build/tiny.gguf` 由 `tools/gen_tiny_model.py`
    生成，以 8.3 名 `/models/tiny.ggu` 放入 ESP。
  - 服务：`aiosd` 常驻，提供 HELLO/CHAT/STATUS/TOOL_LIST/MEMORY/
    AUDIT/MODEL/CANCEL，逐 token 流式事件；`ai` CLI 支持单轮、多轮、
    `exec` 与状态/工具/模型/审计/记忆子命令。
  - Agent：规则意图路由 + 9 个工具（`sys.info/sys.time/sys.disk/
    proc.list/fs.list/fs.read/fs.write/fs.search/calc.eval`），
    `fs.write` 需客户端确认，工具与拒绝均写审计；每会话保留最近若干轮。
- 待实现：Q4/Q5/Q8 反量化与量化矩阵乘、BPE/SentencePiece、KV cache
  快照与上下文摘要/恢复、LRU-K 与向量检索、会话落盘、工具调用次数
  上限、沙箱进程、Shell `?` 集成、桌面 AI 客户端、故障诊断。
- 验收：`make test` 断言 `AI_DAEMON_OK`（守护进程就绪）、
  `AI_MODEL_OK`（内置 GGUF 加载）、`AI_TOOL_OK`（`ai exec "system
  info"` 路由并执行 `sys.info`）、`AI_CHAT_OK`（模型流式回答）；
  `ai status/tools/audit` 可在交互式 Shell 中手工验证。离线运行，
  不依赖网络。

## M10 — 安全加固、可观测性与发布

对应清单：六。

- 系统调用模糊测试、输入长度限制、SMAP/SMEP 回归、沙箱 seccomp 风格白名单。
- dmesg 环形缓冲、统计计数、核心转储、故障自检（AI 生成中文诊断）。
- 单元测试（用户态模拟内核子系统）、CI、发布镜像与文档。
- 验收：fuzz 24h 无 panic；`dmesg` 可导出；AI 故障说明可读且引用真实日志。
