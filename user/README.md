# 用户空间（M7/M8）

```
user/
├── lib/         freestanding C 库：crt0、syscall 包装、string、stdio、stdlib
│   ├── gui.c    桌面协议客户端（fd 3 命令 / fd 4 事件）
│   └── font.c   8×16 位图字体（复用 kernel/font/font8x16.c）
├── programs/    sh、coreutils 与桌面应用
│   ├── desktop.c    合成器、窗口管理、任务栏、开始菜单
│   └── terminal.c files.c editor.c calc.c image.c settings.c taskman.c
├── init.c       PID 1：自检 + fork/exec/pipe/重定向测试 + 启动桌面/shell
└── user.ld      用户程序链接脚本（固定 0x400000，_start 入口）
```

## 构建与安装

`make image` 会：

1. 编译 `user/lib/*.c|*.asm` 为公共对象；
2. 编译 `user/programs/*.c`，每个程序与公共对象链接成独立 ELF；
3. 把 `user/init.c` 链接为 `build/user/init.elf`，用 `tools/gen_blob.py`
   内嵌进内核镜像；
4. 把全部程序以 8.3 大写名复制到 ESP 的 `/BIN`（例如 `SH.ELF`、
   `DESKTOP.ELF`、`TASKMAN.ELF`）。

内核启动后 PID 1 从内嵌 ELF 运行；完成自检后 fork/exec
`/bin/desktop.elf`，父进程再 `execv("/bin/sh.elf")` 保留串口控制台。

## libc 约定

- 头文件：`include/mcube/libc.h`。
- 失败返回 `-1` 并设置 `errno`（系统调用层返回负 errno）。
- `malloc/free` 是 `brk` 上的 bump 分配器：`free` 目前为空操作。coreutils
  生命周期短，桌面/应用通过固定的窗口数量和离屏 surface 控制用量，
  堆上限为 256 MiB；带空闲链表的分配器留待后续里程碑。
- `printf` 支持 `%d/%u/%x/%p/%s/%c/%%` 与宽度/补零，足以覆盖 coreutils
  和桌面应用。

## 桌面协议（M8）

应用不映射 framebuffer，也不创建窗口；它只与桌面交换定长消息
（`include/mcube/gui.h`）：

- fd 3（应用 → 桌面）：`INIT CLEAR FILL RECT TEXT FLUSH TITLE CLOSE
  LAUNCH KILL`。
- fd 4（桌面 → 应用）：`RESIZE KEY MOUSE FOCUS CLOSE TICK`。
- `gui_init(title, w, h)` 发送 `INIT` 并等待桌面分配的 `RESIZE`；
  `gui_wait()` 返回下一个事件并同步窗口宽高。
- 应用用 `gui_clear/gui_fill/gui_rect/gui_text/gui_flush` 在桌面分配的
  离屏 surface 上绘制；`gui_launch(path)` 请桌面启动另一个应用，
  `gui_kill(pid)` 请桌面终止进程。
- 桌面用 `SYS_SPAWN` 启动应用：内核把两个管道端安装为子进程 fd 3/4，
  因此不复制桌面的地址空间。

按键码：可打印字符使用 ASCII；方向键/Delete/Home/End/PgUp/PgDn 与
F1–F12 使用 `include/mcube/input.h` 中的 `0x100` 以上常量。

## Shell 能力

- 分词、单/双引号、反斜杠转义；
- `$VAR`、`${VAR}`、`$?`、`$$` 展开；
- `;` 顺序执行、`|` 管道（最多 4 段）、`>`/`>>`/`<` 重定向、`&` 后台；
- 内建：`cd pwd exit export unset set jobs help echo true false`；
- 外部命令从 `/bin/<name>.elf` 查找，找不到时尝试 `/bin/<name>` 与 `/<name>`。
