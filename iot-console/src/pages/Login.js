import React, { useState, useEffect } from 'react';
import { useHistory, useLocation } from 'react-router-dom';
import { auth, setApiKey, clearApiKey, hasApiKey } from '../api/client';
import './Login.css';

export default function Login() {
  const history = useHistory();
  const location = useLocation();
  const [apiKey, setApiKeyInput] = useState('');
  const [remember, setRemember] = useState(true);
  const [error, setError] = useState('');
  const [loading, setLoading] = useState(false);

  const from = (location.state && location.state.from) ? location.state.from.pathname : '/devices';

  useEffect(() => {
    if (!hasApiKey()) return;
    auth.validate()
      .then(() => history.replace(from))
      .catch(() => {});
  }, [from, history]);

  const handleSubmit = (e) => {
    e.preventDefault();
    setError('');
    if (!apiKey.trim()) {
      setError('请输入 API Key');
      return;
    }
    setLoading(true);
    setApiKey(apiKey.trim(), remember);
    auth.validate()
      .then(() => {
        history.replace(from);
      })
      .catch((err) => {
        clearApiKey();
        setError(err.message || 'API Key 无效或已失效');
        setLoading(false);
      });
  };

  return (
    <div className="login-page">
      <div className="login-card">
        <h1>Yate IoT 管理控制台</h1>
        <p className="login-desc">使用 API Key 登录（X-API-Key / Bearer）</p>
        <form onSubmit={handleSubmit}>
          <div className="form-group">
            <label htmlFor="apikey">API Key</label>
            <input
              id="apikey"
              type="password"
              value={apiKey}
              onChange={(e) => setApiKeyInput(e.target.value)}
              placeholder="请输入 API Key"
              autoComplete="off"
              disabled={loading}
            />
          </div>
          <div className="form-group checkbox">
            <label>
              <input
                type="checkbox"
                checked={remember}
                onChange={(e) => setRemember(e.target.checked)}
              />
              记住登录
            </label>
          </div>
          {error && <div className="login-error">{error}</div>}
          <button type="submit" className="btn-primary" disabled={loading}>
            {loading ? '登录中…' : '登录'}
          </button>
        </form>
        <p className="login-hint">
          首次使用可在网关环境变量中配置 <code>API_KEY</code> 作为引导 Key，或由管理员在「API Key」页创建。
        </p>
      </div>
    </div>
  );
}
