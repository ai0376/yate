import React, { useState, useEffect } from 'react';
import { rules as rulesApi, devices as devicesApi } from '../api/client';
import './Page.css';

const KIND_OPTIONS = ['telemetry', 'attributes'];
const OP_OPTIONS = ['gt', 'gte', 'lt', 'lte', 'eq', 'ne'];

export default function Rules() {
  const [deviceId, setDeviceId] = useState('*');
  const [kind, setKind] = useState('telemetry');
  const [list, setList] = useState([]);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');
  const [createOpen, setCreateOpen] = useState(false);
  const [form, setForm] = useState({
    rule_id: '',
    name: '',
    device_id: '*',
    kind: 'telemetry',
    key_name: '',
    op: 'gt',
    value: '',
    webhook_url: '',
    alarm_level: 'warning',
  });
  const [submitting, setSubmitting] = useState(false);

  const load = () => {
    setLoading(true);
    setError('');
    rulesApi.list(deviceId, kind)
      .then((res) => setList(res.data || []))
      .catch((err) => setError(err.message || '加载失败'))
      .finally(() => setLoading(false));
  };

  useEffect(load, [deviceId, kind]);

  const handleCreate = (e) => {
    e.preventDefault();
    if (!form.rule_id.trim() || !form.key_name.trim()) return;
    setSubmitting(true);
    setError('');
    rulesApi.create(form)
      .then(() => {
        setCreateOpen(false);
        setForm({ rule_id: '', name: '', device_id: '*', kind: 'telemetry', key_name: '', op: 'gt', value: '', webhook_url: '', alarm_level: 'warning' });
        load();
      })
      .catch((err) => setError(err.message || '创建失败'))
      .finally(() => setSubmitting(false));
  };

  const handleDelete = (ruleId) => {
    if (!window.confirm(`确定删除规则 ${ruleId}？`)) return;
    rulesApi.remove(ruleId).then(load).catch((err) => setError(err.message));
  };

  return (
    <div className="page">
      <div className="page-header">
        <h2>规则管理</h2>
        <button type="button" className="btn-primary" onClick={() => setCreateOpen(true)}>
          新建规则
        </button>
      </div>
      <div className="filters">
        <label>
          设备 <input type="text" value={deviceId} onChange={(e) => setDeviceId(e.target.value)} placeholder="* 表示全部" />
        </label>
        <label>
          类型
          <select value={kind} onChange={(e) => setKind(e.target.value)}>
            {KIND_OPTIONS.map((k) => <option key={k} value={k}>{k}</option>)}
          </select>
        </label>
        <button type="button" onClick={load}>查询</button>
      </div>
      {error && <div className="page-error">{error}</div>}
      {createOpen && (
        <div className="modal-overlay" onClick={() => !submitting && setCreateOpen(false)}>
          <div className="modal modal-wide" onClick={(e) => e.stopPropagation()}>
            <h3>新建规则</h3>
            <form onSubmit={handleCreate}>
              <div className="form-row">
                <div className="form-group">
                  <label>规则 ID</label>
                  <input value={form.rule_id} onChange={(e) => setForm({ ...form, rule_id: e.target.value })} placeholder="必填" required />
                </div>
                <div className="form-group">
                  <label>名称</label>
                  <input value={form.name} onChange={(e) => setForm({ ...form, name: e.target.value })} />
                </div>
              </div>
              <div className="form-row">
                <div className="form-group">
                  <label>设备 ID</label>
                  <input value={form.device_id} onChange={(e) => setForm({ ...form, device_id: e.target.value })} placeholder="* 表示全部" />
                </div>
                <div className="form-group">
                  <label>类型</label>
                  <select value={form.kind} onChange={(e) => setForm({ ...form, kind: e.target.value })}>
                    {KIND_OPTIONS.map((k) => <option key={k} value={k}>{k}</option>)}
                  </select>
                </div>
              </div>
              <div className="form-row">
                <div className="form-group">
                  <label>键名</label>
                  <input value={form.key_name} onChange={(e) => setForm({ ...form, key_name: e.target.value })} placeholder="如 temp" required />
                </div>
                <div className="form-group">
                  <label>运算符</label>
                  <select value={form.op} onChange={(e) => setForm({ ...form, op: e.target.value })}>
                    {OP_OPTIONS.map((o) => <option key={o} value={o}>{o}</option>)}
                  </select>
                </div>
                <div className="form-group">
                  <label>阈值</label>
                  <input value={form.value} onChange={(e) => setForm({ ...form, value: e.target.value })} required />
                </div>
              </div>
              <div className="form-group">
                <label>Webhook URL</label>
                <input value={form.webhook_url} onChange={(e) => setForm({ ...form, webhook_url: e.target.value })} placeholder="可选" />
              </div>
              <div className="form-group">
                <label>告警级别</label>
                <input value={form.alarm_level} onChange={(e) => setForm({ ...form, alarm_level: e.target.value })} placeholder="warning" />
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
                <th>规则 ID</th>
                <th>名称</th>
                <th>设备</th>
                <th>类型</th>
                <th>条件</th>
                <th>Webhook</th>
                <th>操作</th>
              </tr>
            </thead>
            <tbody>
              {list.map((r) => (
                <tr key={r.rule_id}>
                  <td>{r.rule_id}</td>
                  <td>{r.name || '-'}</td>
                  <td>{r.device_id}</td>
                  <td>{r.kind}</td>
                  <td>{r.key_name} {r.op} {r.value}</td>
                  <td>{r.webhook_url ? '已配置' : '-'}</td>
                  <td>
                    <button type="button" className="btn-danger btn-sm" onClick={() => handleDelete(r.rule_id)}>删除</button>
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
