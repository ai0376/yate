# IoT 管理平台功能清单与建议增强

本文档对照典型物联网管理平台（如 ThingsBoard）能力，列出**当前已实现**与**建议新增**的功能，便于规划迭代。

---

## 一、当前已具备的能力

| 类别 | 功能 | 说明 |
|------|------|------|
| **接入** | 多协议 | MQTT、CoAP、HTTP 上行（iot-gateway） |
| **设备** | 设备 CRUD | 创建/删除/查询/列表（含分页、total） |
| **设备** | 鉴权 | 基于 token（SHA256 存储），MQTT username=device、password=token |
| **数据** | 上行入库 | telemetry / attributes / heartbeat 写入 `iot_events` |
| **运维** | 集群/微服务 | 多节点 + 共享 DB、可容器化，见 [iot-cluster.md](iot-cluster.md)、[iot-microservices.md](iot-microservices.md) |
| **运维** | 数据可靠性 | 文档说明（DB 持久化、可选 Kafka），见 [iot-reliability.md](iot-reliability.md) |

---

## 二、建议新增功能（按优先级与类别）

### 1. 数据查询与开放（高）✅ 已实现

| 功能 | 说明 | 实现 |
|------|------|------|
| **遥测/属性查询 API** | 按设备、时间范围、kind 查询，分页 | iotdev `iot.event.query`；gateway `GET /api/v1/devices/{id}/telemetry?from=&to=&kind=&limit=&offset=&format=json|csv` |
| **最新值快照** | 某设备全量或按 kind 最新若干条 | iotdev `iot.event.latest`；gateway `GET /api/v1/devices/{id}/telemetry/latest?kind=&limit=` |
| **数据导出** | CSV/JSON 导出 | 查询 API 加 `format=csv` 返回制表符文本，默认 JSON |

### 2. 下行与远程控制（高）✅ 已实现

| 功能 | 说明 | 实现 |
|------|------|------|
| **下行命令 API** | 平台侧下发指令 | HTTP `POST /api/v1/devices/{id}/command` → iotdev `iot.command.send` → 网关 MQTT publish `iot/{id}/cmd`，并 `iot.command.mark_sent` |
| **设备拉取命令** | HTTP 轮询待执行命令 | `GET /api/v1/devices/{id}/commands?token=`（需鉴权），返回 pending 列表并自动 mark_sent |
| **命令历史与状态** | 记录与状态流转 | 表 `iot_commands`（command_id, device_id, payload, status, created_ts, sent_ts, ack_ts, ack_payload）；状态 pending→sent→acked，支持 `iot.command.ack` |

### 3. 规则引擎与告警（中高）✅ 已实现

| 功能 | 说明 | 实现 |
|------|------|------|
| **简单规则引擎** | 根据条件（如 temp > 40）触发动作 | 规则存 `iot_rules`（device_id 可为 `*`）；上行入库后网关拉取 `iot.rule.list`，解析 payload JSON 按 key/op/value 评估，命中则 `iot.alarm.create` 并 POST `webhook_url` |
| **告警定义与状态** | 告警类型、阈值、告警表 | 表 `iot_alarms`（id, device_id, rule_id, start_ts, end_ts, level, acked, ack_ts, payload_snapshot）；`GET /api/v1/devices/{id}/alarms`，`POST /api/v1/alarms/{id}/ack` |
| **通知渠道** | Webhook | 规则命中后网关对 `webhook_url` 发起 POST，body 为 JSON（device, rule_id, kind, ts, level, payload）；可再接钉钉/飞书等 |

### 4. 设备与组织模型（中）

当前无多租户、无设备分组，不利于大规模与权限管理。

| 功能 | 说明 | 实现思路 |
|------|------|----------|
| **租户/项目** | 多租户隔离（tenant_id / project_id） | `iot_devices` 增加 `tenant_id`，所有查询带租户过滤；可选 `iot_tenants` 表 |
| **设备分组/标签** | 按项目、区域、类型分组或打 tag | `iot_devices` 增加 `group_id` 或 `tags`（JSON/关联表）；列表/查询支持按 group/tag 过滤 |
| **设备模板/Profile** | 设备类型、默认属性、上报规范 | 表 `iot_device_profiles`，创建设备时绑定 profile，便于规则与展示按类型区分 |

### 5. 安全与权限（中）

| 功能 | 说明 | 实现思路 |
|------|------|----------|
| **平台 API 鉴权** | 设备用 token；管理/控制台用 API Key 或 JWT | 管理 API（如创建设备、查数据、下发命令）走 API Key 或 OAuth2/JWT，与设备 token 分离 |
| **RBAC** | 角色与权限（只读、运维、管理员） | 用户表 + 角色表 + 权限标识；API 中间件校验权限 |
| **审计日志** | 谁在何时做了哪些操作 | 表 `iot_audit_log`（user/device, action, ts, result）；在创删设备、下发命令、修改配置处落库 |

### 6. 运维与可观测性（中）

| 功能 | 说明 | 实现思路 |
|------|------|----------|
| **设备在线/离线状态** | 基于 last_seen 或心跳判断 | 配置“离线阈值”（如 5 分钟），列表/详情返回 `status: online/offline`；可选定时任务刷状态 |
| **遥测与接入监控** | 写入量、延迟、错误率 | 暴露 Prometheus metrics（gateway + Yate 侧计数）；可选 Grafana 大盘 |
| **健康与就绪** | 网关、Yate、DB 健康检查 | `/healthz`、`/ready` 已部分存在；可增加对 Yate 连接、DB 连接检查 |

### 7. 批量与生命周期（中低）

| 功能 | 说明 | 实现思路 |
|------|------|----------|
| **批量创建设备** | CSV/Excel 导入或 API 批量创建 | `POST /api/v1/devices/bulk` 接受 JSON 数组或文件，循环调用创建设备逻辑，返回成功/失败列表 |
| **设备禁用/启用** | 软禁用，不删数据 | 已有 `enabled` 字段；鉴权与上行校验 enabled，API 增加 PATCH enabled |
| **Token 轮换** | 更换设备 token 不丢历史 | `PUT /api/v1/devices/{id}/token` 更新 token_hash，设备侧需支持重配 token |

### 8. 高级能力（按需）

| 功能 | 说明 | 实现思路 |
|------|------|----------|
| **固件 OTA** | 版本管理、下发升级任务、进度上报 | 新表：固件包、版本、设备版本；任务表；设备上报版本与状态；下行通知“拉取固件” |
| **时序优化** | 海量遥测的存储与查询性能 | 分区表、按时间归档；或对接时序库（InfluxDB/TimescaleDB）；当前 MySQL/PostgreSQL 可先做分区 |
| **可视化大屏** | 设备地图、曲线、状态总览 | 独立前端 + 调用上述查询 API、命令 API；或集成 Grafana 等 |
| **更多协议** | LwM2M、Modbus 等 | 新网关或 Yate 侧协议适配，统一转成 iot.uplink / iot.auth |

---

## 三、建议实施顺序（简要）

1. **第一阶段（数据可用）**：遥测/属性查询 API、最新值接口；平台 API 鉴权（API Key）。
2. **第二阶段（可控）**：下行命令 API、命令历史与状态；设备在线/离线状态。
3. **第三阶段（可运营）**：简单规则与告警、Webhook 通知；租户或设备分组。
4. **第四阶段（可扩展）**：RBAC、审计日志；批量创建、Token 轮换；按需 OTA、时序优化、大屏。

以上顺序可按业务需求调整；文档会随实现情况更新。
