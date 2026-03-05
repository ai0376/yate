import React, { useState, useEffect } from 'react';
import { BrowserRouter as Router, Route, Redirect, Switch, useHistory } from 'react-router-dom';
import { auth, hasApiKey, setApiKey, clearApiKey } from './api/client';
import Login from './pages/Login';
import Layout from './components/Layout';
import Devices from './pages/Devices';
import Rules from './pages/Rules';
import Alarms from './pages/Alarms';
import ApiKeys from './pages/ApiKeys';

function PrivateRoute({ children, ...rest }) {
  const [checking, setChecking] = useState(true);
  const [valid, setValid] = useState(false);

  useEffect(() => {
    if (!hasApiKey()) {
      setChecking(false);
      setValid(false);
      return;
    }
    auth.validate()
      .then(() => { setValid(true); })
      .catch(() => { setValid(false); })
      .finally(() => { setChecking(false); });
  }, []);

  if (checking) {
    return (
      <div className="app-loading">
        校验登录中…
      </div>
    );
  }
  return (
    <Route
      {...rest}
      render={({ location }) =>
        valid ? children : (
          <Redirect to={{ pathname: '/login', state: { from: location } }} />
        )
      }
    />
  );
}

function MainRoutes() {
  const history = useHistory();
  const [user, setUser] = useState(null);

  useEffect(() => {
    auth.validate()
      .then((data) => setUser(data))
      .catch(() => setUser(null));
  }, []);

  const handleLogout = () => {
    clearApiKey();
    setUser(null);
    history.push('/login');
  };

  return (
    <Layout user={user} onLogout={handleLogout}>
      <Switch>
        <Route exact path="/" render={() => <Redirect to="/devices" />} />
        <Route path="/devices" component={Devices} />
        <Route path="/rules" component={Rules} />
        <Route path="/alarms" component={Alarms} />
        <Route path="/apikeys" component={ApiKeys} />
      </Switch>
    </Layout>
  );
}

function App() {
  return (
    <Router>
      <Switch>
        <Route exact path="/login" component={Login} />
        <PrivateRoute path="/">
          <MainRoutes />
        </PrivateRoute>
      </Switch>
    </Router>
  );
}

export default App;
