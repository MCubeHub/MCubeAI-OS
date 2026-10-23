# 用户空间（M7 起）

```
user/
├── lib/         freestanding C 库：crt0、syscall 包装、string、stdio、stdlib
├── programs/    sh 与 coreutils（ls/cat/echo/cp/mv/rm/grep/wc/...）
├── init.c       PID 1：自检 + fork/exec/pipe/重定向测试 + 启动 shell
└── user.ld      用户程序链接脚本（固定 0x400000，_start 入口）
```

## 构建与安装

`make image` 会：

1. 编译 `user/lib/*.c|*.asm` 为公共对象；
2. 编译 `user/programs/*.c`，每个程序与公共对象链接成独立 ELF；
3. 把 `user/init.c` 链接为 `build/user/init.elf`，用 `tools/gen_blob.py`
   内嵌进内核镜像；
4. 把 coreutils 以 8.3 大写名复制到 ESP 的 `/BIN`（例如 `SH.ELF`）。

内核启动后 PID 1 从内嵌 ELF 运行，最后 `execv("/bin/sh.elf")` 交出控制台。
Shell 通过 `/bin/<name>.elf` 解析并 `execve` 外部命令。

## libc 约定

- 头文件：`include/mcube/libc.h`。
- 失败返回 `-1` 并设置 `errno`（系统调用层返回负 errno）。
- `malloc/free` 是 `brk` 上的 bump 分配器：`free` 目前为空操作，
  适合短生命周期的 coreutils；长期运行的 GUI/AI 进程在 M8/M9 换成
  带空闲链表的分配器。
- `printf` 支持 `%d/%u/%x/%p/%s/%c/%%` 与宽度/补零，足以覆盖 coreutils。

## Shell 能力

- 分词、单/双引号、反斜杠转义；
- `$VAR`、`${VAR}`、`$?`、`$$` 展开；
- `;` 顺序执行、`|` 管道（最多 4 段）、`>`/`>>`/`<` 重定向、`&` 后台；
- 内建：`cd pwd exit export unset set jobs help echo true false`；
- 外部命令从 `/bin/<name>.elf` 查找，找不到时尝试 `/bin/<name>` 与 `/<name>`。
