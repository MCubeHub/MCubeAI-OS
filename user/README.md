# 用户空间（M4 起）

计划目录：

```
user/
├── libc/        freestanding C 库（syscall 包装、malloc、printf、pthread）
├── shell/       sh：解析、管道、重定向、作业控制、历史
├── coreutils/   ls/cp/mv/rm/cat/grep/find/ps/free/uname/dmesg...
├── gui/         libui、窗口管理器、合成器、桌面组件
├── apps/        文件管理器、终端、编辑器、计算器、图片查看器、设置、任务管理器
└── ai/          ai CLI、ai-chat 桌面客户端、libai
```

构建产物统一安装到 initrd（`/bin`、`/usr/lib`），由 M5 的 VFS 挂载。
