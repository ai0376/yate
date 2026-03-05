import React from 'react';
import { NavLink, useLocation } from 'react-router-dom';
import './Layout.css';

export default function Layout({ user, onLogout, children }) {
  const location = useLocation();
  const isAdmin = user && user.role === 'admin';

  return (
    <div className="layout">
      <aside className="sidebar">
        <div className="sidebar-title">IoT 控制台</div>
        <nav className="nav">
          <NavLink to="/devices" className="nav-item" activeClassName="active">
            设备
          </NavLink>
          <NavLink to="/rules" className="nav-item" activeClassName="active">
            规则
          </NavLink>
          <NavLink to="/alarms" className="nav-item" activeClassName="active">
            告警
          </NavLink>
          {isAdmin && (
            <NavLink to="/apikeys" className="nav-item" activeClassName="active">
              API Key
            </NavLink>
          )}
        </nav>
        <div className="sidebar-footer">
          <div className="user-info">
            <span className="user-role">{user ? user.role : '-'}</span>
            {user && user.name && <span className="user-name">{user.name}</span>}
          </div>
          <button type="button" className="btn-logout" onClick={onLogout}>
            退出
          </button>
        </div>
      </aside>
      <main className="main">
        {children}
      </main>
    </div>
  );
}
