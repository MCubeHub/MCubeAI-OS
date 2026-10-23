# MCubeAIOS

从零自研的 x86_64 操作系统，目标是把 AI 作为内核之上的**一级系统服务**，
而不是桌面上外挂的一个应用。项目按「启动 → 内核 → 系统服务 → 应用层 → AI 原生层」
分层推进，每个里程碑都有可运行、可验证的交付物。

当前状态：**M7 完成（用户态 libc + Shell + coreutils）**，M8（图形/桌面）进行中。
M0–M6 已交付 UEFI 引导、higher-half 内核、内存/调度/系统调用、块设备/FAT32/VFS/TTY
与 e1000 网络栈；`make test` 覆盖全部里程碑标记。

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
make test       # QEMU 无头启动，断言串口出现 MCUBE_BOOT_OK
make run-gui    # 带窗口启动（需要 X/Wayland，WSLg 可用）
```

在 Windows 侧可这样调用（本项目位于 SMB 挂载的 `Z:\MCubeAIOS`）：

```powershell
wsl -d Debian -- bash -c "cd /mnt/z/MCubeAIOS && make test"
```

## 已实现（M1）

- UEFI 应用引导器：定位 `\kernel.elf`、解析 ELF64、按物理地址装载、解析 GOP/ACPI/memory map。
- 身份映射 64 GiB 页表（2 MiB 大页），切换 CR3 后跳入内核。
- Boot Info Block（版本化、含物理内存图、帧缓冲、RSDP、内核范围和命令行）。
- 串口 16550 驱动 + GOP 8×16 位图字体控制台，双路内核日志。
- 早期 GDT/TSS、256 项 IDT、异常处理与 panic（寄存器、CR2、栈回溯）。
- 8259 PIC 重映射与 IRQ 注册框架；AI 子系统系统调用号段预留。

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
