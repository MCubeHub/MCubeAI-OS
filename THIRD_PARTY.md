# 第三方组件与许可

| 组件 | 用途 | 许可 | 分发形式 |
| --- | --- | --- | --- |
| DejaVu Sans Mono | 内核 8×16 位图字体 | Bitstream Vera / DejaVu 许可（宽松） | 仅嵌入光栅化后的 ASCII 字形数据 `kernel/font/font8x16.c` |
| OVMF | QEMU UEFI 固件（开发依赖，不随镜像分发） | BSD-2-Clause | 运行环境依赖 |
| QEMU | 模拟器（开发依赖） | GPLv2 | 运行环境依赖 |

后续将引入：

- Noto Sans CJK / 文泉驿正黑（SIL OFL / GPL+字体例外）：GUI 中文字体子集。
- stb_truetype / stb_image（MIT/公有领域）：字体与图片解码（若自研成本过高）。
- 本地模型文件（Apache-2.0 等）：默认不随源码分发，由用户自行下载或安装包提供。

自研代码统一使用 MIT 许可。引入新依赖时必须在本文件登记来源、版本、许可与分发方式。
