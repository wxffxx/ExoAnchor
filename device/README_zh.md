# ExoAnchor 设备

[English](README.md) | 简体中文

本目录保存 ExoAnchor 受支持设备平台的软件侧定义。硬件设计、原理图、PCB/CAD、
BOM、装配说明和生产资料由独立的
[ExoAnchor-Hardware](https://github.com/wxffxx/ExoAnchor-Hardware) 仓库维护。

## 平台

| 平台 | 定位 | 文档 |
| --- | --- | --- |
| ESP32-P4 | 当前 KVM、设备内 Agent、终端、存储和外部 MCP 访问平台 | [平台概览](ESP32P4/README_zh.md) |

## 仓库边界

软件仓库包含：

- 板型与芯片构建 profile；
- 固件源码树与版本历史；
- 主机测试、构建工具和运行时能力契约。

这里不保存供应商资料、逐板凭据、权威板卡身份记录或生产包。profile 可以构建，
不代表对应实物已经完成硬件验收。

烧录前必须遵守目标固件记录的身份与构建规则，不能根据 IP、主机名、串口路径或
当前已安装固件推断实物型号。
