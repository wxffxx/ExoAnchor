# Minecraft Deployment Playbook

最终目标之一是稳定部署并管理 Minecraft Java 服务器。

当用户提到“部署 MC server / Minecraft 服务器 / vanilla server / 开服”时，优先生成一个清晰、可验证的多步骤计划，而不是退化成单条命令或硬编码 skill。

## 关键原则

1. 所有 Minecraft 文件都放在一个专用 workload 目录下，例如 `~/.exoanchor/workloads/<workload-dir>/`。
2. 尽量避免把服务文件、jar、世界存档直接散落到 `~`、`/opt` 或其他随机目录。
3. 能不用 root 就不用 root；真正需要 root 的步骤通常只有安装 Java 或开放防火墙。
4. 先保证 Vanilla Java 版可稳定启动，再考虑 Fabric / Forge / Spigot。
5. 如果 Java 安装、systemd、firewall 任一步依赖 sudo，且当前账号没有权限，要明确暴露这个阻塞点，不要伪装成“部署成功”。
6. 如果目标机器上已经存在匹配的 workload，优先复用现有目录和 manifest，不要再凭空创建 `minecraft-server`、`spigot-server` 之外的新目录。

## 推荐目录结构

推荐使用以下结构：

- `~/.exoanchor/workloads/<workload-dir>/server.jar`
- `~/.exoanchor/workloads/<workload-dir>/eula.txt`
- `~/.exoanchor/workloads/<workload-dir>/server.properties`
- `~/.exoanchor/workloads/<workload-dir>/start.sh`
- `~/.exoanchor/workloads/<workload-dir>/launch.sh`
- `~/.exoanchor/workloads/<workload-dir>/server.log`
- `~/.exoanchor/workloads/<workload-dir>/server.pid`
- `~/.exoanchor/workloads/<workload-dir>/manifest.json`

## 推荐部署顺序

1. 检查 Java 是否可用，优先 Java 21。
2. 如果没有 Java，再安装 `openjdk-21-jre-headless` 和 `curl`。
3. 创建 workload 目录。
4. 通过 Mojang/Piston 的官方版本清单下载最新 release 的 `server.jar`。
5. 写入 `eula=true`。
6. 生成 `server.properties`，至少配置 `server-port`、`motd`。
7. 生成 `start.sh`，统一前台启动命令，避免把长命令散落在多个地方。
8. 生成 `launch.sh`，专门负责后台启动、写 `server.pid` 和处理“已经在运行”的情况。
9. 用 `./launch.sh` 启动服务，而不是把复杂后台命令直接塞进 plan 或 manifest。
10. 验证进程、端口、日志。
10. 最后写入 `manifest.json`。

## 下载与启动技巧

- Vanilla 最新版本下载流程应基于 Mojang/Piston manifest，不要硬编码过期的 jar URL。
- `start.sh` 里应使用：
  - `cd <workload-dir>`
  - `exec java -Xms1G -Xmx<MEMORY> -jar server.jar nogui`
- `launch.sh` 应负责：
  - 检查是否已有运行中的 Java 进程
  - 用 `setsid -f ./start.sh > server.log 2>&1 < /dev/null` 后台启动
  - 用 `ps`/`awk` 或等价方式记录真实 Java PID
- 通过 SSH 自动化后台启动时，优先调用 `./launch.sh`，不要只依赖 `nohup ... &`，否则远端 shell 可能不及时退出。
- 如果已有 `server.jar`，优先复用，不要每次重新下载。
- 如果已有运行中的 `server.jar` 进程，应避免重复启动。
- 如果用户是在“修改已有服务器配置”而不是“新部署”，优先查找现有 workload 并直接修改它的 `server.properties` / `launch.sh` / `manifest.json`。

## 验证技巧

部署完成后至少做 3 类验证：

1. 进程验证
   - `pgrep -af 'server.jar|nogui'`
2. 端口验证
   - `ss -tulnp | grep <port>`
3. 日志验证
   - `tail -n 50 server.log`
   - 重点关注 `Done`、`Preparing spawn area`、`Failed to bind to port`、`You need to agree to the EULA`

如果端口监听成功但日志未出现 `Done`，通常说明服务仍在启动中，不要过早判失败。

## 常见失败模式

- `sudo: a password is required`
  - 说明当前账号不能无交互提权，安装 Java 或改防火墙会卡住。
- `Address already in use` / `Failed to bind to port`
  - 说明端口已被占用，应检查旧进程或改端口。
- `You need to agree to the EULA`
  - 说明 `eula.txt` 没写对或目录不对。
- `Unable to access jarfile server.jar`
  - 说明下载失败、工作目录错误，或 `start.sh` 路径不对。
- Java 版本过低
  - 新版 Minecraft 常要求较新的 Java，Ubuntu 上优先尝试 Java 21。

## Manifest 约定

最后一步必须写入 `manifest.json`，推荐：

```json
{"name":"Minecraft (Vanilla)","type":"process","port":25565,"command":"cd ~/.exoanchor/workloads/<workload-dir> && ./launch.sh"}
```

如果将来切换到真正的 systemd 管理，也必须保留 workload 目录和 manifest，只更新 `type` 与 `command`。

## Agent 行为建议

- Minecraft 部署任务默认应该是多步骤计划，不应该退化成单条 `ssh` 命令。
- 如果部署被 sudo 权限阻塞，应明确指出阻塞在“安装依赖/开放端口”，并保留已经成功完成的 workload 文件。
- 如果服务器已经存在，优先做“检查与修复”，不要直接覆盖已有世界数据。
- 如果当前上下文不足以唯一判断要操作哪个现有 Minecraft workload，应该先提问确认，而不是猜目录。
