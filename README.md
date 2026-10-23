# MCubeAIOS

从零自研的 x86_64 操作系统，目标是把 AI 作为内核之上的**一级系统服务**，
而不是桌面上外挂的一个应用。项目按「启动 → 内核 → 系统服务 → 应用层 → AI 原生层」
分层推进，每个里程碑都有可运行、可验证的交付物。

当前状态：**M8 完成（图形子系统与桌面）**。M0–M7 已交付 UEFI 引导、
higher-half 内核、内存/调度/系统调用、块设备/FAT32/VFS/TTY、e1000 网络栈、
用户态 libc/Shell/coreutils；M8 增加帧缓冲映射、PS/2 鼠标、GUI 协议、
集中式合成器与 7 个桌面应用。`make test` 覆盖全部里程碑标记。

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
make run-gui    # 带窗口启动（需要 X/Wayland，WSLg 可用）
make screenshot # 无头启动桌面并截图到 build/screen.ppm（PNG 用 tools/ppm2png.py）
make gui-test   # 用 QEMU monitor 驱动键盘/鼠标，截取菜单、启动、点击三张图
```

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
- **AI 原生层预留**：`0x400–0x417` 系统调用号段与内核钩子已注册，
  `aiosd` 用户态推理守护进程在 M9 实现。

## 目录

| 路径 | 内容 |
| --- | --- |
| `boot/` | UEFI 引导器（自包含 UEFI 头、ELF 装载、分页、交接） |
| `include/mcube/` | 引导器与内核共享的 ABI/类型 |
| `kernel/arch/x86_64/` | 入口、GDT/TSS、IDT/ISR、CPU 原语 |
| `kernel/drivers/` | 串口、帧缓冲控制台（后续 AHCI/键盘/网卡…） |
| `kernel/lib/` | 字符串、printf、panic |
| `kernel/ai/` | AI 子系统内核侧钩子（用户态推理守护进程在 M9） |
| `user/` | libc、shell、coreutils、GUI（M7/M8） |
| `ai/` | `aiosd` 与 AI 运行时（M9） |
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
