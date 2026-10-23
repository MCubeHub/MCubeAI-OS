# 架构设计

## 1. 分层与进程边界

```
┌──────────────────────────────────────────────────────────────┐
│ 桌面应用 / GUI       文件管理器 终端 编辑器 计算器 任务管理器   │
│ AI 客户端 / CLI      chat-ui  ai  ?命令                       │
├──────────────────────────────────────────────────────────────┤
│ 用户态服务           aiosd: 推理线程池 上下文 记忆 工具 审计    │
│                     displayd: 合成器    netd: DHCP/DNS(可选)   │
├──────────────────────────────────────────────────────────────┤
│ libc / libui / libai  POSIX 子集 + 原生 AI 客户端库            │
├──────────────────────────────────────────────────────────────┤
│ 系统调用 ABI          syscall 0x000..0x3FF   ai_* 0x400..0x417 │
├──────────────────────────────────────────────────────────────┤
│ 内核                 VFS 网络 设备 调度 内存 中断 同步 时钟     │
├──────────────────────────────────────────────────────────────┤
│ 引导                 UEFI loader → Boot Info → kernel_main     │
└──────────────────────────────────────────────────────────────┘
```

**为什么 `aiosd` 在用户态**：推理是长时间、可抢占、会失败的普通计算负载，
放进内核会破坏内核的可信基（模型文件解析、GB 级内存、第三方格式都是攻击面）。
内核只保留三件事：`ai_*` 系统调用号段、共享内存/事件通道、审计与权限钩子。
这样 AI 服务崩溃不会带走系统，也能被普通调度器按优先级仲裁。

## 2. 启动流程

1. OVMF 从 FAT32 ESP 加载 `\EFI\BOOT\BOOTX64.EFI`。
2. loader：初始化串口/ConOut → 采集 GOP、RSDP、EfiMemoryMap。
3. loader：从自身设备句柄打开 `\kernel.elf`，解析 ELF64，将每个 `PT_LOAD`
   段按 `p_paddr` 用 `AllocatePages(AllocateAddress)` 预留并拷贝。
4. loader：分配 boot stack、Boot Info 页、内存图副本、页表。
5. loader：建立 64 GiB 身份映射、64 GiB direct map 与 2 GiB 内核窗口
   （2 MiB 大页），填好 Boot Info。
6. loader：`ExitBootServices`（失败则重取 map key 重试）。
7. loader：写 CR3、置 CR0.WP，跳转 `kernel_main(boot_info*)`（高半区）。
8. kernel：早期 GDT → 串口/帧缓冲控制台 → 校验 Boot Info → 打印内存图
   → GDT/TSS → per-CPU/GS → IDT/PIC → 各子系统初始化 → idle。

失败路径统一为「串口 + 屏幕打印错误码并 halt」，绝不静默重启。

## 3. 地址空间规划

| 区域 | 虚拟地址 | 说明 |
| --- | --- | --- |
| 用户空间 | `0x0000_0000_0000_0000 – 0x0000_7FFF_FFFF_FFFF` | 每进程独立 PML4 低半区，含栈/堆/映射 |
| 非规范空洞 | `0x0000_8000_0000_0000 – 0xFFFF_7FFF_FFFF_FFFF` | 禁止映射 |
| direct map | `0xFFFF_8000_0000_0000 + phys` | 全部物理内存线性映射，内核分配器使用 |
| vmalloc | `0xFFFF_C000_0000_0000 – 0xFFFF_CFFF_FFFF_FFFF` | 非连续内核映射 |
| ioremap | `0xFFFF_D000_0000_0000 – 0xFFFF_DFFF_FFFF_FFFF` | MMIO，UC 属性 |
| 内核映像 | `0xFFFF_FFFF_8000_0000` | higher-half，物理装载于 2 MiB |

内核已运行在 higher-half。loader 同时保留低端身份映射供早期引导使用；
用户进程的 PML4 只复制高半区（direct map、vmalloc、内核映像），低半区
完全由进程私有页表填充，因此用户态看不到物理内存和内核数据。

页表属性策略：
- 内核代码 RX、只读数据 R、数据 RW、绝不同时 W+X。
- 用户页 U/S=1，内核页 U/S=0；开启 SMEP/SMAP 后内核不得直接解引用用户指针，
  统一走 `copy_from_user/copy_to_user`。
- 栈页加 guard page；堆页 NX。

## 4. 内核子系统与关键数据结构

| 子系统 | 关键结构 | 说明 |
| --- | --- | --- |
| 物理内存 | `struct page { refcount, flags, order, list }`、位图 | 4 KiB 页帧，伙伴系统 |
| 虚拟内存 | `mm_struct { pml4, vma_list, brk, stack_top }`、`vm_area` | 每进程地址空间 |
| 调度 | `task_struct { pid, state, prio, vruntime, ctx, mm, files, cred }` | RR + MLFQ 接口 |
| 同步 | `spinlock_t`、`mutex_t`、`wait_queue_head_t` | 中断安全自旋锁 |
| VFS | `vfs_mount`、`vfs_inode`、`vfs_file`、`fat32_fs`/`fat32_node` | 最长前缀挂载，fd 表在 `task_t` |
| 块层 | `block_device`、`fat32_fs` | ATA PIO 已接入，AHCI/NVMe 预留 |
| 设备 | `device`、`driver`、`bus_type` | PCI/USB 匹配与电源状态 |
| 网络 | `net_device`、`net_socket`、ARP 表 | e1000（轮询）+ ARP/IPv4/ICMP/UDP/DNS |
| AI 内核侧 | `ai_channel`、`ai_audit_event` | 共享内存环 + 审计队列 |

## 5. 系统调用 ABI

- x86_64：`syscall` 指令；号在 `rax`，参数 `rdi/rsi/rdx/r10/r8/r9`，
  返回值 `rax`，错误为负 errno（`-EFAULT` 等）。
- 入口 `swapgs` 取 per-CPU 块，切到当前任务内核栈（TSS.rsp0 供中断使用），
  保存用户 RIP/RSP/RFLAGS 后调用 C 分发表。
- 返回使用 `sysretq`；用户选择子由 STAR 计算（CS=0x23、SS=0x1b，
  RPL 3）。首次进入用户态用 `iretq` 构造完整帧。
- 所有指针参数先做范围/权限检查，长度上限（如 `PATH_MAX=4096`），
  统一走 `copy_from_user/copy_to_user`，禁止内核直接信任用户结构体。
- `ai_*` 号段（`0x400` 起）：
  `ai_session_create/send/stream_read/cancel/tool_register/status/memory_*`。
- 兼容层：AArch64 使用 `svc #0`，系统调用号表保持一致。
- M5 已落地：`read/write/open/close/stat/fstat/getdents64/mkdir/unlink`
  （号段与 Linux x86_64 对齐）。`mcube_stat_t` 与 `mcube_dirent_t`
  是固定大小的共享 ABI 结构，内核侧对应 `vfs_stat_t`/`vfs_dirent_t`。
- 文件描述符 0/1/2 保留给控制台；3..15 由每任务 fd 表分配。
  `read(0)` 从 TTY 环形队列取数据，空队列返回 `-EAGAIN`。
- M6 已落地：`socket(41)/connect(42)/sendto(44)/recvfrom(45)/bind(49)`。
  socket 与文件共用 fd 空间：`task_t->files[]` 存 VFS 句柄，
  `task_t->socks[]` 存 socket 对象，分配时两者都为空才算空闲；
  任务退出时 `net_task_cleanup()` 回收。地址结构 `mcube_sockaddr_in_t`
  与 Linux `sockaddr_in` 线格式一致（网络字节序）。

## 6. 文件系统与设备模型

- VFS 统一 `open/read/write/stat/getdents64`；当前按打开实例分配 inode，
  dentry/page cache 与 LRU 留待后续。
- 首版实际文件系统选择 FAT32（UEFI 可直接挂载，便于自举）：
  superfloppy 或 MBR 首分区、512 字节扇区、8.3 目录项、双 FAT 同步。
  tmpfs 承载 `/tmp`，devfs 提供 `/dev/{console,tty0,ttyS0,null,zero}`。
- 块层当前为同步 ATA PIO（主通道 LBA28）；分区扫描（MBR，GPT 预留）、
  请求合并、超时重试与统计随 AHCI/NVMe 一起补齐。
- `/dev` 由 devfs 在挂载时提供固定设备节点；主/次设备号与权限位待扩展。
- TTY 输入来自 PS/2 IRQ1 与 COM1 IRQ4；行规程、PTY 与控制台切换待实现。
- 网络自底向上：PCI legacy 配置空间枚举 → e1000 MMIO 驱动
  （32 项 RX/TX 环、IMS=0 纯轮询）→ `net_receive()` 分发 ARP/IPv4 →
  ICMP/UDP/DNS。ARP 表 16 项，发送路径按掩码选择直连或网关。
  TCP 在 M6.1 接入，DHCP/IPv6 随后。

## 7. AI 原生层接口

```
应用/CLI ──Unix socket(JSON/CBOR)──> aiosd ──ai_* syscall──> kernel
                                      │
                                      ├─ 推理线程池（绑定 CPU 配额）
                                      ├─ 上下文管理器（KV cache 快照）
                                      ├─ 记忆/存储管理器（文件 + 向量索引）
                                      └─ 工具管理器（经 Access Manager 鉴权）
```

- 流式协议：`{"id":..,"event":"token|tool_call|tool_result|done|error","data":..}`，
  每个会话一个双向流；取消通过 `cancel` 消息 + 内核事件双通道。
- 上下文切换：推理任务被抢占时保存 KV cache 索引与消息偏移，
  下次恢复只重算未完成的部分，不重跑 prompt。
- 记忆分级：会话记忆（RAM，LRU-K）→ 项目记忆（文件）→ 用户长期偏好（文件+向量库）。
- 审计：每次工具调用写入 `journal`，包含 agent uid、工具名、参数摘要、
  结果状态、用户授权记录；支持撤销授权。

## 8. 安全模型

- 内核/用户隔离、SMEP/SMAP、W^X、栈保护、`copy_*_user` 边界检查。
- AI Agent 使用独立 UID + 能力集（capability bits），默认只能读自己的目录。
- 工具调用三档权限：`allow`（只读查询）、`confirm`（写文件/执行命令）、
  `deny`（密钥、系统配置、网络外联，除非显式开启）。
- 沙箱执行：`ai-sandbox` 子进程，seccomp 风格系统调用白名单 + 路径白名单。
- 提示注入：工具返回值标记为不可信内容，模型侧包裹隔离标记，UI 上单独展示。

## 9. 构建与验证

- 交叉/原生工具链：`gcc` + `nasm` + `x86_64-w64-mingw32-gcc`（UEFI PE）。
- 内核链接地址 2 MiB（M1）→ higher-half（M2）；保留 `.symtab` 供 gdb。
- `make test`：QEMU 无头启动 + 串口断言 + 超时保护，CI 可直接运行。
- 用户态子系统（slab、VFS 路径解析、调度器）提供 host 侧单元测试，
  不依赖 QEMU，保证快速回归。
