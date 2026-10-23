# 验收矩阵

规则：每条验收项都有 ID、验证方式与硬性通过条件。里程碑只有在表中对应行
全部通过后才能标记完成。`make test` 是自动化入口，手工项标注为 `manual`。

## 一、启动与引导层（M1）

| ID | 项目 | 验证方式 | 通过条件 |
| --- | --- | --- | --- |
| B-01 | UEFI 引导 | `make test` | 串口出现 `[MCUBOOT]` 日志与 `MCUBE_BOOT_OK` |
| B-02 | 内核 ELF 装载 | loader 日志 | 打印 kernel entry/size，入口等于链接脚本地址 |
| B-03 | GOP 帧缓冲 | `manual` / 截图 | 屏幕显示 banner，分辨率与 QEMU 设置一致 |
| B-04 | 内存图交接 | 启动日志 | 内存图条目 > 0，usable + reserved ≈ 总内存 |
| B-05 | ACPI RSDP | 启动日志 | 签名 `RSD PTR ` 校验通过 |
| B-06 | 异常处理 | 注入 `#PF`（调试构建） | 打印 CR2/错误码/栈回溯并 halt，不重启 |
| B-07 | 串口调试 | `make test` | 115200 8N1，日志无乱码 |
| B-08 | 失败回退 | 删除 kernel.elf 后启动 | 屏幕/串口打印错误并 halt |

## 二、内核核心层（M2–M4）

| ID | 项目 | 验证方式 | 通过条件 |
| --- | --- | --- | --- |
| K-01 | 物理页分配 | 内核自测 `mtest pmm` | 分配/释放 100k 页后空闲页数恢复，无泄漏 |
| K-02 | 虚拟内存 | `mtest vmm` | direct map/vmalloc/ioremap 读写正确，权限位符合预期 |
| K-03 | 内核堆 | `mtest slab` | 随机大小 alloc/free 10 万次无越界、无泄漏 |
| K-04 | 地址空间隔离 | 用户态测试 | 用户读写内核页触发 `#PF`；SMAP/SMEP 生效 |
| K-05 | COW fork | `mtest cow` | 子进程写后父进程数据不变，物理页引用计数正确 |
| K-06 | 缺页处理 | 用户程序 | 合法 VMA 补页成功；非法地址收到 SIGSEGV |
| K-07 | 上下文切换 | `mtest sched` | 1000 次切换寄存器/FPU 状态一致 |
| K-08 | 调度公平性 | `top` 采样 | 两个 CPU 密集进程时间占比偏差 < 10% |
| K-09 | 系统调用 | `syscall-test` | 非法指针/长度返回 `-EFAULT/-EINVAL`，不 panic |
| K-10 | 同步原语 | `mtest lock` | 4 线程计数无丢失；锁顺序反转被检测 |
| K-11 | 定时器 | `clock-test` | `nanosleep(100ms)` 误差 < 5 ms；单调时钟不倒退 |
| K-12 | SMP | `make test SMP=2` | 双核上线，per-CPU idle 正常，IPI 工作 |
| K-13 | 模块框架 | `manual` | 静态模块注册/卸载钩子可枚举，依赖检查生效 |

## 三、系统服务层（M5–M6）

| ID | 项目 | 验证方式 | 通过条件 |
| --- | --- | --- | --- |
| S-01 | VFS 基本操作 | `vfs-test` | open/read/write/lseek/stat/rename/unlink 全部通过 |
| S-02 | tmpfs | `ls /tmp` | 重启后清空，读写正确，容量限制生效 |
| S-03 | FAT32 | `fat-test` | 创建/删除/重命名/大文件读写后 fsck 无错误 |
| S-04 | 块设备 | `hdparm` 等价测试 | AHCI 识别磁盘，DMA 读写校验和一致 |
| S-05 | 分区表 | `fdisk -l` 等价 | MBR 分区正确解析，越界访问被拒绝 |
| S-06 | TTY 行规程 | `manual` | 退格/Ctrl+C/Ctrl+D/Ctrl+Z 行为符合预期 |
| S-07 | 多控制台 | `Alt+F1/F2` | 切换后各自 Shell 状态独立 |
| S-08 | 键盘/鼠标 | `make gui-test` | 按键映射正确，鼠标位移/滚轮事件可达桌面（部分：`/dev/input` 节点待补） |
| S-09 | 设备节点 | `ls -l /dev` | 主/次设备号正确，权限检查生效 |
| S-10 | 网络 ICMP | `ping 10.0.2.2` | 丢包率 0%，RTT 合理 |
| S-11 | UDP/TCP | `nc` 等价测试 | 回环与宿主双向传输校验和一致 |
| S-12 | socket API | `net-test` | bind/connect/listen/accept/send/recv 全通过 |

## 四、用户空间层（M7–M8）

| ID | 项目 | 验证方式 | 通过条件 | 状态 |
| --- | --- | --- | --- | --- |
| U-01 | libc 内存/字符串 | `make libc-test` | 单元测试全绿，malloc/free 无泄漏 | 部分：string/stdio/stdlib 已随 coreutils 运行，host 单测待补 |
| U-02 | printf | `libc-test` | 格式化输出与 glibc 对比一致（覆盖宽度/精度） | 部分：`%d/%u/%x/%p/%s/%c` 与宽度/补零已用 |
| U-03 | Shell 解析 | `sh-test` | 引号、转义、变量、PATH 查找正确 | 通过：`sh -c` 与交互式实测 |
| U-04 | 管道/重定向 | `manual` | `ls / | grep bin | wc -l` 输出正确 | 通过：`ls /bin | wc -l`、`echo x > /tmp/f` 回读 |
| U-05 | 作业控制 | `manual` | Ctrl+Z/bg/fg/jobs 行为正确 | 部分：`&` 后台与 `jobs` 已实现，Ctrl+Z/bg/fg 待补 |
| U-06 | 历史/补全 | `manual` | 上下键浏览，Tab 文件名补全 | 待实现 |
| U-07 | coreutils | `coreutils-test` | 文件/文本/系统工具基准用例通过 | 部分：20 个工具可用，基准脚本待补 |
| U-08 | 行编辑器 | `manual` | 打开、编辑、保存、退出，非法输入不崩溃 | 通过：Editor 应用，F2 保存回 FAT32 |
| U-09 | 帧缓冲双缓冲 | 截图 | 无撕裂，窗口拖动无残影 | 部分：应用离屏 surface + 合成器 blit；垂直同步/拖动待补 |
| U-10 | 字体 | 截图 | 英文清晰，内置常用汉字可显示 | 部分：8×16 位图英文字体；中文子集待补 |
| U-11 | 窗口管理 | `manual` | 创建/拖动/缩放/最小化/关闭/Z 序/焦点正确 | 部分：平铺/焦点/关闭/任务栏已通过；拖动/缩放/最小化待补 |
| U-12 | 输入路由 | `make gui-test` | 键盘鼠标事件只送达焦点窗口 | 通过：F1 菜单、方向键、回车、PS/2 点击均验证 |
| U-13 | libui | `ui-test` | 按钮/文本框/列表/菜单回调触发正确 | 部分：`gui.c` 客户端 + 应用内控件；独立控件库待补 |
| U-14 | 桌面组件 | 截图 | 壁纸、任务栏、开始菜单、时钟、托盘可见可用 | 通过：`make screenshot` |
| U-15 | 桌面应用 | `manual` | 文件管理器/终端/编辑器/计算器/图片/设置/任务管理器可用 | 通过：7 个应用随桌面启动并绘制 |
| U-16 | 应用启动隔离 | `make gui-test` | 新应用获得独立地址空间，不复制桌面堆 | 通过：`SYS_SPAWN` 启动，任务管理器可见独立 pid |
| U-17 | 桌面协议 | `make test` | 全部窗口完成 INIT+FLUSH | 通过：断言 `USER_DESKTOP_OK`、`USER_GUI_OK` |

## 五、AI 原生层（M9）

| ID | 项目 | 验证方式 | 通过条件 |
| --- | --- | --- | --- |
| A-01 | 模型加载 | `ai model load` | GGUF Q4/Q5/Q8 加载成功，损坏文件被拒绝 |
| A-02 | 推理正确性 | 基准 prompt | 输出语义正确，与参考实现 logits 误差 < 1e-3 |
| A-03 | 性能 | `ai status` | Qwen2.5-1.5B Q4_K_M 在 4 vCPU QEMU > 3 tok/s |
| A-04 | 流式输出 | `ai chat` | 首 token < 2 s，逐 token 渲染 |
| A-05 | 采样参数 | 对比测试 | temperature/top-k/top-p/penalty 生效且可复现 |
| A-06 | 上下文截断 | 长对话 | 超限触发摘要，后续回答仍引用最近 4 轮 |
| A-07 | 上下文恢复 | 中止后继续 | 恢复不重跑 prompt，已生成 token 保留 |
| A-08 | 记忆 LRU-K | `mtest ai-mem` | 超容量淘汰顺序符合 LRU-K，落盘可恢复 |
| A-09 | 向量检索 | `ai memory search` | top-3 命中相关文档，余弦分数正确 |
| A-10 | 会话归档 | 重启 aiosd | `ai sessions` 可恢复最近会话 |
| A-11 | 工具调用 | `ai exec` | 正确选择工具、校验参数、回填结果并继续推理 |
| A-12 | 工具上限 | 恶意 prompt | 超过 8 次调用/64 KiB 结果被截断并报错 |
| A-13 | 权限确认 | `ai exec "写文件"` | 弹出确认，拒绝后不执行，审计记录完整 |
| A-14 | 沙箱 | 逃逸测试 | 访问白名单外路径/系统调用被拒绝 |
| A-15 | 提示注入 | 注入样本 | 工具结果被标记为不可信，敏感操作要求确认 |
| A-16 | 审计 | `ai audit --tail` | 每次工具调用都有 uid/工具/参数摘要/结果/耗时 |
| A-17 | 系统助手 | `?` 命令 | 自然语言正确转成系统操作并经确认执行 |
| A-18 | 命令纠错 | 错误命令 | 给出修正建议，确认后执行成功 |
| A-19 | 语义文件搜索 | `ai search` | 按描述找到目标文件，而非仅字符串匹配 |
| A-20 | 桌面客户端 | `manual` | 多轮对话、Markdown、停止、重新生成、附件入口可用 |
| A-21 | 故障诊断 | 注入 panic | 错误界面显示中文说明与日志引用 |
| A-22 | 离线 | 断网运行 | 默认模型推理与工具（本地类）不受影响 |

## 六、安全与工程化（贯穿，M10 收口）

| ID | 项目 | 验证方式 | 通过条件 |
| --- | --- | --- | --- |
| E-01 | 系统调用 fuzz | 24h 运行 | 无 panic、无内存破坏，异常输入返回 errno |
| E-02 | 内核日志 | `dmesg` | 环形缓冲可读、可清空、溢出策略正确 |
| E-03 | 统计计数 | `/proc/stat` 等价 | 中断/I-O 错误/分配失败/切换次数单调递增 |
| E-04 | 核心转储 | 用户崩溃 | 生成 core 文件并可被分析工具读取 |
| E-05 | 单元测试 | `make test-host` | 内存/VFS/调度器模拟测试全绿 |
| E-06 | 调试 | `make debug` | gdb 可断点内核、符号完整 |
| E-07 | 镜像复现 | 两次构建 | 相同输入产出相同 kernel.elf（时间戳除外） |
