import React, { useState, useEffect } from 'react';
import { devices as devicesApi } from '../api/client';
import './Page.css';

export default function Devices() {
  const [list, setList] = useState({ data: [], total: 0 });
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [page, setPage] = useState(0);
  const [limit] = useState(20);
  const [createOpen, setCreateOpen] = useState(false);
  const [form, setForm] = useState({ device: '', token: '', name: '' });
  const [submitting, setSubmitting] = useState(false);

  const load = () => {
    setLoading(true);
    setError('');
    devicesApi.list({ limit, offset: page * limit })
      .then((res) => {
        setList({ data: res.data || [], total: res.total || 0 });
      })
      .catch((err) => setError(err.message || '加载失败'))
      .finally(() => setLoading(false));
  };

  useEffect(load, [page, limit]);

  const handleCreate = (e) => {
    e.preventDefault();
    if (!form.device.trim()) return;
    setSubmitting(true);
    setError('');
    devicesApi.create({
      device: form.device.trim(),
      token: form.token.trim() || form.device.trim(),
      name: form.name.trim() || form.device.trim(),
    })
      .then(() => {
        setCreateOpen(false);
        setForm({ device: '', token: '', name: '' });
        load();
      })
      .catch((err) => setError(err.message || '创建设备失败'))
      .finally(() => setSubmitting(false));
  };

  const handleDelete = (id) => {
    if (!window.confirm(`确定删除设备 ${id}？`)) return;
    devicesApi.remove(id).then(load).catch((err) => setError(err.message));
  };

  const totalPages = Math.ceil(list.total / limit) || 1;

  return (
    <div className="page">
      <div className="page-header">
        <h2>设备管理</h2>
        <button type="button" className="btn-primary" onClick={() => setCreateOpen(true)}>
          创建设备
        </button>
      </div>
      {error && <div className="page-error">{error}</div>}
      {createOpen && (
        <div className="modal-overlay" onClick={() => !submitting && setCreateOpen(false)}>
          <div className="modal" onClick={(e) => e.stopPropagation()}>
            <h3>创建设备</h3>
            <form onSubmit={handleCreate}>
              <div className="form-group">
                <label>设备 ID</label>
                <input
                  value={form.device}
                  onChange={(e) => setForm({ ...form, device: e.target.value })}
                  placeholder="必填"
                  required
                />
              </div>
              <div className="form-group">
                <label>Token</label>
                <input
                  value={form.token}
                  onChange={(e) => setForm({ ...form, token: e.target.value })}
                  placeholder="可选，默认与设备 ID 相同"
                />
              </div>
              <div className="form-group">
                <label>名称</label>
                <input
                  value={form.name}
                  onChange={(e) => setForm({ ...form, name: e.target.value })}
                  placeholder="可选"
                />
              </div>
              <div className="modal-actions">
                <button type="button" onClick={() => setCreateOpen(false)} disabled={submitting}>
                  取消
                </button>
                <button type="submit" className="btn-primary" disabled={submitting}>
                  {submitting ? '提交中…' : '创建'}
                </button>
              </div>
            </form>
          </div>
        </div>
      )}
      <div className="table-wrap">
        {loading ? (
          <div className="loading">加载中…</div>
        ) : (
          <table className="table">
            <thead>
              <tr>
                <th>设备 ID</th>
                <th>名称</th>
                <th>启用</th>
                <th>最后在线</th>
                <th>操作</th>
              </tr>
            </thead>
            <tbody>
              {list.data.map((row) => (
                <tr key={row.device_id}>
                  <td>{row.device_id}</td>
                  <td>{row.name || '-'}</td>
                  <td>{row.enabled ? '是' : '否'}</td>
                  <td>{row.last_seen ? new Date(row.last_seen * 1000).toLocaleString() : '-'}</td>
                  <td>
                    <button
                      type="button"
                      className="btn-danger btn-sm"
                      onClick={() => handleDelete(row.device_id)}
                    >
                      删除
                    </button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>
      {list.total > limit && (
        <div className="pagination">
          <button
            type="button"
            disabled={page <= 0}
            onClick={() => setPage((p) => p - 1)}
          >
            上一页
          </button>
          <span>第 {page + 1} / {totalPages} 页，共 {list.total} 条</span>
          <button
            type="button"
            disabled={page >= totalPages - 1}
            onClick={() => setPage((p) => p + 1)}
          >
            下一页
          </button>
        </div>
      )}
    </div>
  );
}
