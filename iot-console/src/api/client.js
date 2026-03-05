/**
 * 前端 API 模块：统一封装后端接口，使用 API Key 鉴权
 */

const STORAGE_KEY = 'iot_console_api_key';
const AUTH_HEADER = 'X-API-Key';

function getBaseUrl() {
  if (process.env.NODE_ENV === 'development' && process.env.REACT_APP_API_URL) {
    return process.env.REACT_APP_API_URL;
  }
  return process.env.REACT_APP_API_URL || '';
}

function getApiKey() {
  return localStorage.getItem(STORAGE_KEY) || sessionStorage.getItem(STORAGE_KEY) || '';
}

export function setApiKey(key, remember = true) {
  if (remember) {
    localStorage.setItem(STORAGE_KEY, key);
    sessionStorage.removeItem(STORAGE_KEY);
  } else {
    sessionStorage.setItem(STORAGE_KEY, key);
    localStorage.removeItem(STORAGE_KEY);
  }
}

export function clearApiKey() {
  localStorage.removeItem(STORAGE_KEY);
  sessionStorage.removeItem(STORAGE_KEY);
}

export function hasApiKey() {
  return !!getApiKey();
}

async function request(method, path, body = null, opts = {}) {
  const url = path.startsWith('http') ? path : `${getBaseUrl()}${path}`;
  const headers = {
    'Content-Type': 'application/json',
    [AUTH_HEADER]: getApiKey(),
    ...opts.headers,
  };
  const config = { method, headers };
  if (body && method !== 'GET') {
    config.body = typeof body === 'string' ? body : JSON.stringify(body);
  }
  const res = await fetch(url, config);
  if (res.status === 401) {
    clearApiKey();
    const err = new Error(res.statusText || '未授权');
    err.status = 401;
    throw err;
  }
  const text = await res.text();
  if (!res.ok) {
    const err = new Error(text || res.statusText);
    err.status = res.status;
    throw err;
  }
  if (!text) return null;
  try {
    return JSON.parse(text);
  } catch (_) {
    return text;
  }
}

// ---- Auth 模块 ----
export const auth = {
  validate() {
    return request('GET', '/api/v1/auth/validate');
  },
};

// ---- Devices 模块 ----
export const devices = {
  list(params = {}) {
    const q = new URLSearchParams(params).toString();
    return request('GET', `/api/v1/devices${q ? '?' + q : ''}`);
  },
  get(id) {
    return request('GET', `/api/v1/devices/${id}`);
  },
  create(data) {
    return request('POST', '/api/v1/devices', data);
  },
  remove(id) {
    return request('DELETE', `/api/v1/devices/${id}`);
  },
  telemetry(id, params = {}) {
    const q = new URLSearchParams(params).toString();
    return request('GET', `/api/v1/devices/${id}/telemetry${q ? '?' + q : ''}`);
  },
  telemetryLatest(id, params = {}) {
    const q = new URLSearchParams(params).toString();
    return request('GET', `/api/v1/devices/${id}/telemetry/latest${q ? '?' + q : ''}`);
  },
  alarms(id, params = {}) {
    const q = new URLSearchParams(params).toString();
    return request('GET', `/api/v1/devices/${id}/alarms${q ? '?' + q : ''}`);
  },
  sendCommand(id, payload) {
    return request('POST', `/api/v1/devices/${id}/command`, payload);
  },
};

// ---- Rules 模块 ----
export const rules = {
  list(deviceId, kind) {
    return request('GET', `/api/v1/rules?device_id=${encodeURIComponent(deviceId)}&kind=${encodeURIComponent(kind)}`);
  },
  create(data) {
    return request('POST', '/api/v1/rules', data);
  },
  remove(ruleId) {
    return request('DELETE', `/api/v1/rules/${encodeURIComponent(ruleId)}`);
  },
};

// ---- Alarms 模块 ----
export const alarms = {
  ack(alarmId) {
    return request('POST', `/api/v1/alarms/${alarmId}/ack`);
  },
};

// ---- API Keys 模块（仅 admin） ----
export const apikeys = {
  list() {
    return request('GET', '/api/v1/apikeys');
  },
  create(data) {
    return request('POST', '/api/v1/apikeys', data);
  },
  remove(keyId) {
    return request('DELETE', `/api/v1/apikeys/${encodeURIComponent(keyId)}`);
  },
};
