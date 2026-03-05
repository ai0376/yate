import React, { useState, useEffect } from 'react';
import { devices as devicesApi, alarms as alarmsApi } from '../api/client';
import './Page.css';

export default function Alarms() {
  const [deviceId, setDeviceId] = useState('');
  const [deviceList, setDeviceList] = useState([]);
  const [list, setList] = useState([]);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');
  const [activeOnly, setActiveOnly] = useState(true);

  useEffect(() => {
    devicesApi.list({ limit: 500 }).then((res) => setDeviceList(res.data || [])).catch(() => {});
  }, []);

  const load = () => {
    if (!deviceId.trim()) {
      setList([]);
      return;
    }
    setLoading(true);
    setError('');
    const params = {};
    if (activeOnly) params.active_only = 'true';
    devicesApi.alarms(deviceId, params)
      .then((res) => setList(res.data || []))
      .catch((err) => setError(err.message || '加载失败'))
      .finally(() => setLoading(false));
  };

  useEffect(load, [deviceId, activeOnly]);

  const handleAck = (alarmId) => {
    alarmsApi.ack(alarmId).then(load).catch((err) => setError(err.message));
  };

  return (
    <div className="page">
      <div className="page-header">
        <h2>告警列表</h2>
      </div>
      <div className="filters">
        <label>
          设备
          <select value={deviceId} onChange={(e) => setDeviceId(e.target.value)}>
            <option value="">请选择设备</option>
            {deviceList.map((d) => (
              <option key={d.device_id} value={d.device_id}>{d.device_id} {d.name ? `(${d.name})` : ''}</option>
            ))}
          </select>
        </label>
        <label>
          <input type="checkbox" checked={activeOnly} onChange={(e) => setActiveOnly(e.target.checked)} />
          仅未恢复
        </label>
        <button type="button" onClick={load}>查询</button>
      </div>
      {error && <div className="page-error">{error}</div>}
      <div className="table-wrap">
        {loading ? <div className="loading">加载中…</div> : (
          <table className="table">
            <thead>
              <tr>
                <th>ID</th>
                <th>设备</th>
                <th>规则</th>
                <th>级别</th>
                <th>开始时间</th>
                <th>确认</th>
                <th>操作</th>
              </tr>
            </thead>
            <tbody>
              {list.map((a) => (
                <tr key={a.id}>
                  <td>{a.id}</td>
                  <td>{a.device_id}</td>
                  <td>{a.rule_id}</td>
                  <td>{a.level}</td>
                  <td>{a.start_ts ? new Date(a.start_ts * 1000).toLocaleString() : '-'}</td>
                  <td>{a.acked ? '已确认' : '未确认'}</td>
                  <td>
                    {!a.acked && (
                      <button type="button" className="btn-primary btn-sm" onClick={() => handleAck(a.id)}>
                        确认
                      </button>
                    )}
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
