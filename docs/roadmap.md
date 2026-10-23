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

## M2 — 物理/虚拟内存（进行中）

对应清单：二.2、二.6 的一部分。

- 物理页帧分配器（位图 + 每 CPU 缓存），预留内核/loader/MMIO/ACPI。
- 四级页表管理：direct map、vmalloc、ioremap；内核切换 higher-half。
- 内核堆：slab 对象缓存 + 伙伴系统，`kmalloc/kfree`，调试模式泄漏跟踪。
- 每进程地址空间、COW、缺页处理（合法补映射 / 非法 SIGSEGV）。
- SMAP/SMEP、NX、只读页与 W^X。
- 验收：`mtest` 内核自测通过（分配/释放 10 万页无泄漏、COW 写时复制、
  非法访问触发 panic 或 SIGSEGV、`/proc/meminfo` 数据一致）。

## M3 — 中断、时钟与 SMP

对应清单：二.1、二.5、二.4（自旋锁）。

- LAPIC + IOAPIC、APIC timer、HPET/TSC 校准、RTC。
- 下半部：softirq/tasklet/workqueue；中断上下文约束检查。
- 自旋锁（含关中断）、信号量、mutex、等待队列、原子操作。
- ACPI MADT 解析与 AP 启动（INIT/SIPI），每 CPU 数据。
- 系统调用 `time/clock_gettime/nanosleep/alarm`。
- 验收：双核启动、`ps` 显示 per-CPU idle、定时器误差 < 1%、
  锁顺序反转在调试模式被检测并 panic。

## M4 — 进程、调度与系统调用

对应清单：二.3、二.6、三.6、四.1 的 syscall 包装。

- PCB/TCB、fork/exec/exit/wait/kill、线程 create/join/exit。
- RR 调度 + MLFQ 接口，FPU 惰性切换，上下文切换汇编。
- `syscall` 入口（MSR STAR/LSTAR/FMASK）、参数校验、负 errno。
- 用户态 ELF 装载器、最小 init（PID 1）。
- 验收：init 运行并打印；`fork` 1000 次无泄漏；`execve` 运行静态 ELF；
  非法指针/超大长度系统调用返回 `-EFAULT/-EINVAL` 而非 panic。

## M5 — 文件系统、块设备、TTY

对应清单：三.1–三.5。

- VFS：inode/dentry/file/superblock、page cache、路径解析。
- tmpfs（initrd）与 FAT32 读写；MBR 分区表；日志文件系统预留。
- AHCI（DMA）与 NVMe 基础读写、块层请求队列与错误重试。
- TTY/PTY：行规程、Ctrl+C/Z/D、前后台进程组、多控制台。
- PS/2 键盘鼠标、`/dev/input/*`、`/dev` 设备节点与权限。
- 验收：`mount`、`ls/cat/cp` 在 FAT32 镜像上成功；断电模拟后 FAT 不损坏；
  Ctrl+C 能终止前台进程；`/dev/ttyS0` 可读写。

## M6 — 网络

对应清单：三.4。

- e1000 驱动、ARP/IPv4/ICMP/UDP/TCP（简化可靠传输）。
- socket/bind/connect/listen/accept/send/recv、DNS 解析。
- 验收：`ping 10.0.2.2` 通；TCP echo 回环与跨 QEMU 主机通；
  `ifconfig` 显示正确 MAC/IP/统计。

## M7 — 用户空间基础（libc + Shell + coreutils）

对应清单：四.1–四.3。

- libc：内存/字符串/printf/errno/syscall 包装/pthread 轻量线程。
- Shell：分词、引号、变量、PATH、管道/重定向/后台、作业控制、历史、Tab。
- coreutils 子集与行编辑器。
- 验收：交互式 Shell 完成管道 `ls / | grep bin | wc -l`；
  `Ctrl+Z`/`bg`/`fg` 正常；`make libc-test` 全部通过。

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
