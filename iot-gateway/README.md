## yate-iot-gateway（MQTT/CoAP/HTTP → Yate）

这个网关进程负责把多协议设备上行统一转成 Yate `extmodule` 协议消息，并调用 Yate 内部的 `iotdev` 模块完成鉴权与入库。

**集群部署**：可多节点部署，需使用共享 MySQL/PostgreSQL；详见 [docs/iot-cluster.md](../docs/iot-cluster.md)。

### 运行前提

- **Yate 已编译/运行**，并加载：
  - `modules/server/iotdev.yate`
  - `modules/extmodule.yate`
  - 一个数据库模块（推荐 `modules/server/sqlitedb.yate`）
- 需要在 Yate 配置中：
  - `sqlitedb.conf` 里配置数据库账号 `[iot]`（可参考 `conf.d/sqlitedb-iot.conf.sample`）
  - `iotdev.conf`（可参考 `conf.d/iotdev.conf.sample`）
  - `extmodule.conf` 增加一个 TCP listener（可参考 `conf.d/extmodule-iotgw.conf.sample`）

### 构建（需要 Go 1.21+）

在仓库根目录：

```bash
cd iot-gateway
go mod tidy
go build -o yate-iot-gateway ./cmd/yate-iot-gateway
```

### 启动

```bash
./yate-iot-gateway --yate=127.0.0.1:5040 --mqtt=:1883 --http=:8088 --coap=:5683
```

### 数据接入大小限制

| 协议 | 网关侧限制 | 说明 |
|------|------------|------|
| **HTTP** | 默认 **1MB**，可配置 | 单次 POST body 上限，通过 `--http-max-body=N` 或环境变量 `HTTP_MAX_BODY` 设置（字节），范围 256～32MB。 |
| **MQTT** | 由内嵌 broker 决定 | 一般单条消息可达数 MB，具体见 MQTT 服务配置。 |
| **CoAP** | 单 UDP 包 + 分块 | 单次请求受 UDP 包与 libcoap 分块限制，大 payload 会走 block-wise。 |

**Yate iotdev 模块** 会对入库的 payload 再做一次截断，由 `iotdev.conf` 的 `[general] max_payload` 控制（默认 **8192** 字节，最大可配到 **1MB**）。若需要接入更大单条数据，请同时调大：

1. 网关：`--http-max-body=1048576`（或更大，不超过 32MB）
2. Yate：`conf.d/iotdev.conf` 中 `max_payload=1048576`

### 设备管理（HTTP）

- **创建设备**

```bash
curl -sS -X POST "http://127.0.0.1:8088/api/v1/devices" \
  -H "Content-Type: application/json" \
  -d '{"device":"d1","token":"t1","name":"demo"}'
```

- **列出设备（分页，支持 10 万级）**

```bash
# 默认返回前 10000 条，并带 total 总数
curl -sS "http://127.0.0.1:8088/api/v1/devices"
# 分页：每页 1000 条，第 2 页
curl -sS "http://127.0.0.1:8088/api/v1/devices?limit=1000&offset=1000"
```
响应格式：`{"total": 100000, "data": [{"device_id":"...","name":"...","enabled":1,"last_seen":0}, ...]}`

### 上行遥测（HTTP）

```bash
curl -sS -X POST "http://127.0.0.1:8088/api/v1/d1/telemetry?token=t1" \
  -H "Content-Type: application/json" \
  -d '{"temp":23.4,"hum":40}'
```

### 上行遥测（MQTT）

网关内嵌 MQTT broker（默认 `:1883`），使用 **username=deviceId**、**password=token** 进行连接鉴权。

- 发布遥测（示例 topic：`iot/<device>/telemetry`）

```bash
# 以 mosquitto_pub 为例
mosquitto_pub -h 127.0.0.1 -p 1883 -u d1 -P t1 -t "iot/d1/telemetry" -m '{"temp":25}'
```

### 上行遥测（CoAP）

```bash
coap-client -m post "coap://127.0.0.1:5683/api/v1/d1/telemetry?token=t1" -e '{"temp":22}'
```

### 遥测/事件查询（高优先级）

- **按设备、时间、类型分页查询**

```bash
# 查询设备 d1 的遥测，时间范围、类型、分页
curl -sS "http://127.0.0.1:8088/api/v1/devices/d1/telemetry?from=1609459200&to=1640995200&kind=telemetry&limit=100&offset=0"
# 导出 CSV
curl -sS "http://127.0.0.1:8088/api/v1/devices/d1/telemetry?from=1609459200&limit=1000&format=csv"
```
查询参数：`from`、`to` 为 Unix 秒（或毫秒，自动识别）；`kind` 可选 telemetry/attributes/heartbeat；`limit`、`offset` 分页。响应 JSON：`{"total": N, "data": [{"id","device_id","ts","kind","proto","payload"}, ...]}`。

- **最新值快照**

```bash
curl -sS "http://127.0.0.1:8088/api/v1/devices/d1/telemetry/latest"
# 仅某类型最新一条
curl -sS "http://127.0.0.1:8088/api/v1/devices/d1/telemetry/latest?kind=telemetry&limit=1"
```

### 下行命令（高优先级）

- **平台侧下发一条命令**（写入 Yate 后经 MQTT 推送到设备订阅的 `iot/{device}/cmd`）

```bash
curl -sS -X POST "http://127.0.0.1:8088/api/v1/devices/d1/command" \
  -H "Content-Type: application/json" \
  -d '{"action":"reboot","delay":5}'
# 响应：{"command_id":"...", "result":"ok"}
```

- **设备拉取待执行命令**（HTTP 轮询，需设备 token 鉴权；拉取后自动标记为已发送）

```bash
curl -sS "http://127.0.0.1:8088/api/v1/devices/d1/commands?token=t1"
# 或 Header: X-Token: t1
# 响应：{"data":[{"command_id","device_id","payload","status","created_ts"}, ...]}
```

设备确认执行后可调用 Yate 的 `iot.command.ack`（或后续扩展 HTTP 确认接口）更新命令状态为已确认。

### 规则引擎与告警（中高优先级）

- **规则**：按设备/类型配置阈值条件，遥测入库后自动评估；命中则写告警并可选触发 Webhook。

- **创建规则**（`device_id='*'` 表示所有设备）：

```bash
curl -sS -X POST "http://127.0.0.1:8088/api/v1/rules" \
  -H "Content-Type: application/json" \
  -d '{
    "rule_id": "temp_high",
    "name": "温度过高",
    "device_id": "*",
    "kind": "telemetry",
    "key_name": "temp",
    "op": "gt",
    "value": "40",
    "webhook_url": "https://your-server.com/webhook",
    "alarm_level": "warning"
  }'
```

- **列出规则**（按设备 + kind 查询）：

```bash
curl -sS "http://127.0.0.1:8088/api/v1/rules?device_id=d1&kind=telemetry"
```

- **删除规则**：

```bash
curl -sS -X DELETE "http://127.0.0.1:8088/api/v1/rules/temp_high"
```

- **告警列表**（某设备）：

```bash
curl -sS "http://127.0.0.1:8088/api/v1/devices/d1/alarms"
# 仅未恢复：?active_only=true
```

- **确认告警**：

```bash
curl -sS -X POST "http://127.0.0.1:8088/api/v1/alarms/1/ack"
```

- **条件运算符**：`gt` / `gte` / `lt` / `lte` / `eq` / `ne`；`key_name` 为遥测 JSON 的顶层 key（如 `temp`）。命中后写入 `iot_alarms` 并可选对 `webhook_url` 发起 POST，body 为 JSON：`device`、`rule_id`、`kind`、`ts`、`level`、`payload`。

