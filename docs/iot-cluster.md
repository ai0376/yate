# IoT 平台集群化部署说明

当前项目**可以做集群化部署**。数据可靠性说明（含是否需引入 Kafka）见 [iot-reliability.md](iot-reliability.md)。微服务化部署见 [iot-microservices.md](iot-microservices.md)。

集群前提是使用**共享数据库**（MySQL/PostgreSQL），并合理规划网关与 Yate 的对应关系。

---

## 1. 架构要点

- **Yate**：单进程、单节点，无内置 IoT 多节点协调；多节点 = 多台机器各跑一个 Yate。
- **iotdev**：状态全部在数据库；只要多个 Yate 使用**同一套库表**（同一 MySQL/PostgreSQL 账号），即形成“多节点共享同一设备/遥测数据”的集群。
- **iot-gateway**：每个进程连**一个** Yate（extmodule TCP）；无状态，可水平扩展，但需明确“哪个网关连哪个 Yate”。

---

## 2. 推荐集群拓扑

```
                    [负载均衡 / 外部 MQTT Broker]
                                    |
        +----------------+----------+----------+----------------+
        |                |                    |                |
   [Gateway-1]      [Gateway-2]           [Gateway-N]     ...
        |                |                    |
   [Yate-1]         [Yate-2]             [Yate-N]
        |                |                    |
        +----------------+----------+----------+
                                    |
                    [MySQL / PostgreSQL 共享库]
```

- **数据库**：所有 Yate 节点的 iotdev 使用**同一** MySQL 或 PostgreSQL 的同一库、同一账号（如 `account=iot`），实现设备/遥测数据共享。
- **网关**：每个网关进程连一个 Yate（`--yate=host:port`）；可通过 DNS、LB 或固定列表把不同网关指到不同 Yate，实现水平扩展和故障隔离。
- **接入层**：
  - **HTTP/CoAP**：前接 LB（如 Nginx/HAProxy），后端挂多个 gateway 的 HTTP/CoAP 端口，LB 将请求转到任意一台 gateway 即可（无会话状态）。
  - **MQTT**：当前网关内嵌 broker，连接绑在单进程。集群下更稳妥的方式是使用**外部 MQTT 集群**（如 EMQX），由网关作为客户端订阅主题并转发到 Yate；或“每 Yate 一个 gateway”、对外用 LB 做 TCP 转发到不同 gateway 的 MQTT 端口。

---

## 3. 必须满足的条件

| 项目 | 说明 |
|------|------|
| **不用 SQLite 做共享** | SQLite 单文件、单写，无法被多台 Yate 共享。集群必须用 **MySQL 或 PostgreSQL**，并在 mysqldb.conf / pgsqldb.conf 中配置同一 [iot] 账号指向同一库。 |
| **每节点独立配置** | 每个 Yate 节点本机加载 iotdev + extmodule + mysqldb/pgsqldb，且 `iotdev.conf` 里 `database.account=iot` 指向该共享库。 |
| **网关 ↔ Yate 对应** | 每个 iot-gateway 的 `--yate` 指向某一台 Yate；扩节点时增加 (Yate + gateway) 对，并在 LB 或 DNS 上做分流。 |

---

## 4. 配置示例（多节点共享 MySQL）

- **Yate 节点**（每台相同）：  
  - 加载 `mysqldb.yate`（或 pgsqldb）、`iotdev.yate`、`extmodule.yate`。  
  - `mysqldb.conf` 中配置账号 `[iot]`，指向**同一** MySQL 实例与库。  
  - `iotdev.conf` 中 `[database] account=iot`。  
  - `extmodule.conf` 中为本机开一个 listener（如 `addr=0.0.0.0 port=5040`），供本机或 LB 后的 gateway 连接。

- **网关**：  
  - 每进程一个 Yate，例如 `--yate=yate-node1:5040`；多台网关可分别指向 `yate-node1`、`yate-node2` 等。  
  - HTTP/CoAP 端口可同端口多实例（不同机器）或同机不同端口，由 LB 统一暴露。

- **LB**：  
  - 对 HTTP：反向代理到多台 gateway 的 8088（或你配置的端口）。  
  - 对 CoAP：UDP 可做 DNS 轮询或 LB 的 UDP 转发到多台 gateway 的 5683。

---

## 5. 限制与注意

- **MQTT 会话**：设备连的是“某台 gateway 的内嵌 broker”。若要做高可用，需用外部 MQTT 集群 + 网关订阅转发，或接受“单 gateway 故障时其 MQTT 连接断开”。  
- **下行命令**：当前下行经 gateway 内嵌 broker 发到已连设备；设备固定连某台 gateway 时，下行需落到同一 Yate/gateway，否则需在应用层做“设备 → 所在 gateway”的路由。  
- **Yate 自身**：Yate 的 clustering 模块面向**话务路由**，与 IoT 设备接入无关；IoT 集群不依赖它，只要共享 DB + 多组 (gateway, Yate) 即可。

---

## 6. 小结

- **可以集群化部署**：多 (Yate + gateway) + 共享 MySQL/PostgreSQL。  
- **必须**：集群内全部使用 **MySQL 或 PostgreSQL** 作为 iotdev 后端，且各节点指向同一库。  
- **扩展方式**：增加 Yate 节点与对应 gateway，在接入层用 LB 或外部 MQTT 做分流即可。
