## yate-iot-gateway（MQTT/CoAP/HTTP → Yate）

这个网关进程负责把多协议设备上行统一转成 Yate `extmodule` 协议消息，并调用 Yate 内部的 `iotdev` 模块完成鉴权与入库。

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

