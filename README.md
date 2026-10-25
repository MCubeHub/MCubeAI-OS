# MCubeAIOS

从零自研的 x86_64 操作系统，目标是把 AI 作为内核之上的**一级系统服务**，
而不是桌面上外挂的一个应用。项目按「启动 → 内核 → 系统服务 → 应用层 → AI 原生层」
分层推进，每个里程碑都有可运行、可验证的交付物。

当前状态：**M9 基线完成（AI 原生层）**。M0–M8 已交付 UEFI 引导、
higher-half 内核、内存/调度/系统调用、块设备/FAT32/VFS/TTY、e1000 网络栈、
用户态 libc/Shell/coreutils、帧缓冲/合成器与 7 个桌面应用；M9 增加常驻
AI 守护进程 `aiosd`、GGUF v3 加载与本地推理、流式输出、规则意图路由、
9 个内置工具、确认与审计、`ai` CLI，并由 PID 1 做端到端自检。
`make test` 覆盖全部里程碑标记。

```
                 ┌─────────────────────────────────────────────┐
  用户态         │ coreutils · shell · GUI · 桌面应用           │
                 │ aiosd（推理/上下文/记忆/工具/访问控制）       │
                 ├─────────────────────────────────────────────┤
  系统服务       │ VFS · FAT32/tmpfs · AHCI · TTY · 网络协议栈   │
                 ├─────────────────────────────────────────────┤
  内核           │ 调度 · 虚拟内存 · 中断/APIC · 同步 · 系统调用 │
                 ├─────────────────────────────────────────────┤
  引导           │ UEFI loader · Boot Info Block · 早期控制台    │
                 └─────────────────────────────────────────────┘
```

## 快速开始

构建与运行在 Linux（本仓库开发环境为 WSL2 Debian 12）中完成：

```bash
# 依赖
sudo apt install build-essential nasm qemu-system-x86 xorriso mtools gdb \
                 gcc-mingw-w64-x86-64 ovmf python3

make            # 生成 build/esp.img（FAT32 EFI 系统分区）
make test       # QEMU 无头启动，断言串口出现全部里程碑标记
make iso        # UEFI El Torito ISO（也可 dd 到 U 盘）
make disk       # MBR 磁盘镜像（硬盘/U 盘通用）+ VHD/QCOW2
make disk-gpt   # 同一布局的 GPT 版本
make test-disk  # 从分区硬盘镜像启动并跑完整验收
make test-usb   # 模拟 U 盘（xHCI + USB mass storage）启动
make test-iso   # 从 ISO 启动
make run-gui    # 带窗口启动（需要 X/Wayland，WSLg 可用）
make screenshot # 无头启动桌面并截图到 build/screen.ppm（PNG 用 tools/ppm2png.py）
make gui-test   # 用 QEMU monitor 驱动键盘/鼠标，截取菜单、启动、点击三张图
```

所有镜像共用同一条启动链：UEFI loader 读取 `\INITRD.IMG`（内置 kernel、
`/bin/*.lgy`、AI 模型）并交给内核；内核优先挂载启动盘上的 FAT32 ESP，
找不到时自动挂载 initrd tmpfs 作为 `/`。因此 U 盘和 ISO 不依赖内核里的
USB/光驱驱动也能进入完整桌面；硬盘镜像则通过 MBR/GPT 分区扫描挂载
`sda1`，自检写入的 `/TEST.TXT` 会持久保存在分区内。

`make test` 由 `tools/test.sh` 驱动：出现最后一个标记 `USER_GUI_OK` 后立即
退出并断言全部里程碑标记；`RUN_TIMEOUT`（默认 90 秒）只是异常时的上限。

在 Windows 侧可这样调用（本项目位于 SMB 挂载的 `Z:\MCubeAIOS`）：

```powershell
wsl -d Debian -- bash -c "cd /mnt/z/MCubeAIOS && make test"
```

## 已实现

- **M1–M2 引导与内核**：UEFI 应用引导器定位 `\kernel.elf`、解析 ELF64、
  按物理地址装载、交接 GOP/ACPI/memory map；身份映射与 higher-half 迁移；
  Boot Info Block、串口 16550 + GOP 8×16 字体控制台、GDT/TSS/IDT、异常
  处理与 panic、8259 PIC 重映射。
- **M3–M4 内存与调度**：伙伴系统物理页、4 KiB 页表管理、内核堆、
  per-task 地址空间、fork/exec 的用户态切换、RR 调度与系统调用入口。
- **M5–M6 系统服务**：ATA PIO 块层、FAT32（8.3 名、双 FAT）、VFS
  （tmpfs/devfs/FAT 最长前缀挂载）、TTY、PCI 枚举、e1000 轮询驱动、
  ARP/IPv4/ICMP/UDP/DNS。
- **M7 用户空间**：freestanding libc、fork/execve/wait4/pipe/dup2/brk、
  20 个 coreutils、带管道/重定向/后台作业/变量展开的 Shell。
- **M8 图形与桌面**：`SYS_FB_INFO/FB_MAP` 把 GOP 帧缓冲映射到用户态，
  PS/2 键盘 + 鼠标 IRQ12 事件队列，集中式合成器（桌面独占 framebuffer，
  应用使用离屏 surface + 管道协议），窗口平铺/焦点/关闭、任务栏、
  开始菜单、时钟、鼠标光标，以及 Terminal/Files/Editor/Calculator/
  Image Viewer/Settings/Task Manager 七个应用。
- **M9 AI 原生层（基线）**：内核提供两条匿名管道组成的会话通道与
  `0x400–0x417` 系统调用号段；用户态 `aiosd` 加载 GGUF v3 模型
  （内置 256 字节词表的 tiny bigram，F32 张量），执行 llama 风格
  前向、RMSNorm 与 top-k/温度采样，逐 token 流式返回。规则路由把
  自然语言映射到 9 个工具（系统信息/时间/磁盘/进程/目录/读文件/
  写文件/搜索/计算），写文件需客户端确认，所有工具调用写入内核审计环。
  `ai` CLI 支持单轮、多轮、`exec`、`status/tools/model/audit/memory`。
- **后续里程碑**：Q4/Q5/Q8 量化模型、BPE 词表、上下文摘要与恢复、
  LRU-K/向量记忆、会话归档、桌面 AI 客户端、Shell `?` 集成与沙箱。

## 目录

| 路径 | 内容 |
| --- | --- |
| `boot/` | UEFI 引导器（自包含 UEFI 头、ELF 装载、分页、交接） |
| `include/mcube/` | 引导器与内核共享的 ABI/类型 |
| `kernel/arch/x86_64/` | 入口、GDT/TSS、IDT/ISR、CPU 原语 |
| `kernel/drivers/` | 串口、帧缓冲控制台（后续 AHCI/键盘/网卡…） |
| `kernel/lib/` | 字符串、printf、panic |
| `kernel/ai/` | AI 会话通道、审计环与守护进程注册（内核侧） |
| `user/` | libc、shell、coreutils、GUI 与 `ai` 客户端（M7–M9） |
| `ai/` | `aiosd` 使用的 GGUF/推理运行时、工具与会话记忆（M9） |
| `docs/` | 架构、路线图、验收矩阵、AI 设计 |
| `tools/` | 字体生成、QEMU 运行、环境检查 |

## 文档

- [`docs/roadmap.md`](docs/roadmap.md)：里程碑与功能清单映射
- [`docs/architecture.md`](docs/architecture.md)：分层架构、地址空间、启动流程、ABI
- [`docs/ai-native.md`](docs/ai-native.md)：AI 原生层设计（推理、上下文、记忆、工具、安全）
- [`docs/acceptance.md`](docs/acceptance.md)：逐层最小验收标准

## 原则

1. **可启动优先**：每个里程碑结束都必须 `make test` 通过，禁止留下不能开机的中间态。
2. **A/B 双路径**：关键子系统同时保留硬件路径与 QEMU 验证路径。
3. **离线优先**：AI 默认本地推理，无网络也能完成对话与工具调用。
4. **权限最小化**：AI Agent 以独立 UID/沙箱运行，工具调用经过内核权限检查与审计。

许可证：MIT（自研代码）。第三方字体/协议实现许可见 `THIRD_PARTY.md`。
