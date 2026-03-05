import React, { useState, useEffect } from 'react';
import { apikeys as apikeysApi } from '../api/client';
import './Page.css';

export default function ApiKeys() {
  const [list, setList] = useState([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [createOpen, setCreateOpen] = useState(false);
  const [form, setForm] = useState({ key_id: '', name: '', role: 'operator' });
  const [submitting, setSubmitting] = useState(false);
  const [createdKey, setCreatedKey] = useState(null);

  const load = () => {
    setLoading(true);
    setError('');
    apikeysApi.list()
      .then((res) => setList(res.data || []))
      .catch((err) => setError(err.message || '加载失败'))
      .finally(() => setLoading(false));
  };

  useEffect(load, []);

  const handleCreate = (e) => {
    e.preventDefault();
    setSubmitting(true);
    setError('');
    setCreatedKey(null);
    apikeysApi.create(form)
      .then((res) => {
        setCreatedKey(res);
        setForm({ key_id: '', name: '', role: 'operator' });
        load();
      })
      .catch((err) => setError(err.message || '创建失败'))
      .finally(() => setSubmitting(false));
  };

  const handleDelete = (keyId) => {
    if (!window.confirm(`确定删除 Key ${keyId}？`)) return;
    apikeysApi.remove(keyId).then(load).catch((err) => setError(err.message));
  };

  const copyKey = () => {
    if (createdKey && createdKey.api_key) {
      navigator.clipboard.writeText(createdKey.api_key);
      alert('已复制到剪贴板');
    }
  };

  return (
    <div className="page">
      <div className="page-header">
        <h2>API Key 管理</h2>
        <button type="button" className="btn-primary" onClick={() => { setCreateOpen(true); setCreatedKey(null); }}>
          创建 Key
        </button>
      </div>
      {error && <div className="page-error">{error}</div>}
      {createdKey && createdKey.api_key && (
        <div className="alert alert-success">
          <strong>创建成功。</strong> API Key（仅显示一次，请妥善保存）：
          <code className="api-key-display">{createdKey.api_key}</code>
          <button type="button" className="btn-primary btn-sm" onClick={copyKey}>复制</button>
        </div>
      )}
      {createOpen && (
        <div className="modal-overlay" onClick={() => !submitting && setCreateOpen(false)}>
          <div className="modal" onClick={(e) => e.stopPropagation()}>
            <h3>创建 API Key</h3>
            <form onSubmit={handleCreate}>
              <div className="form-group">
                <label>Key ID（可选，不填自动生成）</label>
                <input value={form.key_id} onChange={(e) => setForm({ ...form, key_id: e.target.value })} placeholder="如 my-key" />
              </div>
              <div className="form-group">
                <label>名称</label>
                <input value={form.name} onChange={(e) => setForm({ ...form, name: e.target.value })} placeholder="可选" />
              </div>
              <div className="form-group">
                <label>角色</label>
                <select value={form.role} onChange={(e) => setForm({ ...form, role: e.target.value })}>
                  <option value="admin">admin</option>
                  <option value="operator">operator</option>
                  <option value="readonly">readonly</option>
                </select>
              </div>
              <div className="modal-actions">
                <button type="button" onClick={() => setCreateOpen(false)} disabled={submitting}>取消</button>
                <button type="submit" className="btn-primary" disabled={submitting}>{submitting ? '提交中…' : '创建'}</button>
              </div>
            </form>
          </div>
        </div>
      )}
      <div className="table-wrap">
        {loading ? <div className="loading">加载中…</div> : (
          <table className="table">
            <thead>
              <tr>
                <th>Key ID</th>
                <th>名称</th>
                <th>角色</th>
                <th>启用</th>
                <th>创建时间</th>
                <th>操作</th>
              </tr>
            </thead>
            <tbody>
              {list.map((k) => (
                <tr key={k.key_id}>
                  <td>{k.key_id}</td>
                  <td>{k.name || '-'}</td>
                  <td>{k.role}</td>
                  <td>{k.enabled ? '是' : '否'}</td>
                  <td>{k.created_ts ? new Date(k.created_ts * 1000).toLocaleString() : '-'}</td>
                  <td>
                    <button type="button" className="btn-danger btn-sm" onClick={() => handleDelete(k.key_id)}>删除</button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>
    </div>
  );
}
