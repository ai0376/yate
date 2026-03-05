# Yate IoT 管理控制台（React 16 前端）

基于 React 16 的物联网管理前端，提供登录、设备/规则/告警/API Key 管理页面。后端接口由 **iot-gateway** 提供，新增的登录校验等接口以后端**模块化**方式实现（如 `GET /api/v1/auth/validate`）。

## 依赖

- Node.js 14+
- npm 或 yarn

## 安装与运行

```bash
cd iot-console
npm install
npm start
```

开发环境下默认通过 `package.json` 的 `proxy` 将 API 请求转发到 `http://127.0.0.1:8088`，请先启动 **iot-gateway**。

## 构建

```bash
npm run build
```

生成物在 `build/`，可部署到任意静态服务器。若前端与网关不同域，需在网关注册 CORS 或通过 Nginx 反向代理到同一域。

## 环境变量

| 变量 | 说明 |
|------|------|
| `REACT_APP_API_URL` | 后端 API 根地址（如 `http://localhost:8088`）。不设且开发模式下使用 proxy。 |

## 登录

- 使用 **API Key** 登录（与网关管理 API 鉴权一致）。
- 支持请求头 `X-API-Key` 或 `Authorization: Bearer <key>`；登录后 Key 保存在前端（本地存储或会话存储），后续请求自动携带。
- 首次使用可在网关配置环境变量 `API_KEY` 作为引导 Key，或由管理员在控制台「API Key」页创建 Key 后使用。

## 后端接口（模块化）

前端依赖的接口均在 **iot-gateway** 中以模块形式实现：

- **Auth 模块**：`GET /api/v1/auth/validate` — 校验当前 API Key，返回 `key_id`、`role`、`name`，供登录与权限展示。
- **Devices**：`GET/POST /api/v1/devices`、`GET/DELETE /api/v1/devices/:id`、遥测/告警子路径等（已有）。
- **Rules**：`GET/POST /api/v1/rules`、`DELETE /api/v1/rules/:id`（已有）。
- **Alarms**：`GET /api/v1/devices/:id/alarms`、`POST /api/v1/alarms/:id/ack`（已有）。
- **API Keys**：`GET/POST /api/v1/apikeys`、`DELETE /api/v1/apikeys/:id`（已有）。

前端通过 `src/api/client.js` 统一封装上述接口，按需扩展即可。
