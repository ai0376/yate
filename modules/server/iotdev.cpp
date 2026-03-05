/**
 * iotdev.cpp
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * IoT device management core (transport-agnostic).
 *
 * Provides:
 *  - Device registry and token authentication
 *  - Unified uplink ingestion (telemetry/attributes/heartbeat/rpc)
 *  - Persistence via the existing "database" message (sqlitedb/mysqldb/pgsqldb)
 *
 * Intended to be used with extmodule-based protocol gateways (MQTT/CoAP/HTTP...).
 */

#include <yatephone.h>

using namespace TelEngine;

namespace {

static inline String sqlEscape(const String& in)
{
    if (!in)
	return String("");
    String out;
    for (const char* p = in.c_str(); p && *p; ++p) {
	if (*p == '\'')
	    out << "''";
	else
	    out << *p;
    }
    return out;
}

static inline int64_t parseTs(const String& ts, int64_t fallbackSec)
{
    if (!ts)
	return fallbackSec;
    int64_t v = ts.toInt64(-1);
    if (v <= 0)
	return fallbackSec;
    // If it looks like milliseconds since epoch convert to seconds
    if (v > 20000000000LL)
	v /= 1000;
    return v;
}

class IotDevModule;

class IotAuthHandler : public MessageHandler
{
public:
    inline IotAuthHandler(unsigned int prio = 100)
	: MessageHandler("iot.auth",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotUplinkHandler : public MessageHandler
{
public:
    inline IotUplinkHandler(unsigned int prio = 100)
	: MessageHandler("iot.uplink",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotDeviceCreateHandler : public MessageHandler
{
public:
    inline IotDeviceCreateHandler(unsigned int prio = 100)
	: MessageHandler("iot.device.create",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotDeviceDeleteHandler : public MessageHandler
{
public:
    inline IotDeviceDeleteHandler(unsigned int prio = 100)
	: MessageHandler("iot.device.delete",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotDeviceGetHandler : public MessageHandler
{
public:
    inline IotDeviceGetHandler(unsigned int prio = 100)
	: MessageHandler("iot.device.get",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotDeviceListHandler : public MessageHandler
{
public:
    inline IotDeviceListHandler(unsigned int prio = 100)
	: MessageHandler("iot.device.list",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotDevModule : public Module
{
public:
    IotDevModule();
    virtual ~IotDevModule();
    virtual void initialize();
    bool unload();

    bool handleAuth(Message& msg);
    bool handleUplink(Message& msg);
    bool handleDeviceCreate(Message& msg);
    bool handleDeviceDelete(Message& msg);
    bool handleDeviceGet(Message& msg);
    bool handleDeviceList(Message& msg);

private:
    bool dbDispatch(Message& db);
    bool dbExec(const String& queryTmpl, const NamedList& params, bool results, Message& out);
    bool dbInit();
    bool dbGetDevice(const String& deviceId, String& tokenHash, String& name, int& enabled, int64_t& lastSeen);
    bool dbUpdateLastSeen(const String& deviceId, int64_t nowSec);
    bool dbInsertEvent(const String& deviceId, const String& kind, const String& proto, int64_t tsSec, const String& payload);

    bool authDevice(const String& deviceId, const String& token, bool updateLastSeen, String* err = 0);

    bool m_init;
    String m_accountDB;
    int m_maxPayload;
    bool m_allowUnauth;
    bool m_autoCreateTables;

    // SQL templates
    String m_sqlCreateDevices;
    String m_sqlCreateEvents;
    String m_sqlInsertDevice;
    String m_sqlDeleteDevice;
    String m_sqlSelectDevice;
    String m_sqlUpdateLastSeen;
    String m_sqlInsertEvent;
    String m_sqlListDevices;
    String m_sqlListDevicesCount;
    int m_defaultListLimit;
    int m_maxListLimit;

    // Handlers
    IotAuthHandler* m_hAuth;
    IotUplinkHandler* m_hUplink;
    IotDeviceCreateHandler* m_hDevCreate;
    IotDeviceDeleteHandler* m_hDevDelete;
    IotDeviceGetHandler* m_hDevGet;
    IotDeviceListHandler* m_hDevList;
};

INIT_PLUGIN(IotDevModule);

UNLOAD_PLUGIN(unloadNow)
{
    if (unloadNow && !__plugin.unload())
	return false;
    return true;
}

IotDevModule::IotDevModule()
    : Module("iotdev","misc"),
      m_init(false),
      m_maxPayload(8192),
      m_allowUnauth(false),
      m_autoCreateTables(true),
      m_defaultListLimit(10000),
      m_maxListLimit(50000),
      m_hAuth(0),
      m_hUplink(0),
      m_hDevCreate(0),
      m_hDevDelete(0),
      m_hDevGet(0),
      m_hDevList(0)
{
    Output("Loaded module IoT Device Core");
}

IotDevModule::~IotDevModule()
{
    Output("Unloaded module IoT Device Core");
    TelEngine::destruct(m_hAuth);
    TelEngine::destruct(m_hUplink);
    TelEngine::destruct(m_hDevCreate);
    TelEngine::destruct(m_hDevDelete);
    TelEngine::destruct(m_hDevGet);
    TelEngine::destruct(m_hDevList);
}

void IotDevModule::initialize()
{
    if (m_init)
	return;
    m_init = true;

    Configuration cfg(Engine::configFile("iotdev"));
    cfg.load();

    m_accountDB = cfg.getValue("database","account","iot");
    m_maxPayload = cfg.getIntValue("general","max_payload",8192,256,1024*1024,false);
    m_allowUnauth = cfg.getBoolValue("general","allow_unauth",false);
    m_autoCreateTables = cfg.getBoolValue("database","auto_create_tables",true);

    // Defaults are SQLite-friendly but should work on most engines.
    m_sqlCreateDevices = cfg.getValue("database","create_devices",
	"CREATE TABLE IF NOT EXISTS iot_devices ("
	" device_id TEXT PRIMARY KEY,"
	" token_hash TEXT NOT NULL,"
	" name TEXT,"
	" enabled INTEGER NOT NULL DEFAULT 1,"
	" created_at INTEGER NOT NULL,"
	" updated_at INTEGER NOT NULL,"
	" last_seen INTEGER"
	");");

    m_sqlCreateEvents = cfg.getValue("database","create_events",
	"CREATE TABLE IF NOT EXISTS iot_events ("
	" id INTEGER PRIMARY KEY AUTOINCREMENT,"
	" device_id TEXT NOT NULL,"
	" ts INTEGER NOT NULL,"
	" kind TEXT NOT NULL,"
	" proto TEXT,"
	" payload TEXT"
	");"
	"CREATE INDEX IF NOT EXISTS idx_iot_events_device_ts ON iot_events(device_id,ts);");

    m_sqlInsertDevice = cfg.getValue("database","insert_device",
	"INSERT INTO iot_devices(device_id,token_hash,name,enabled,created_at,updated_at,last_seen)"
	" VALUES('${device}','${token_hash}','${name}',1,${now},${now},${now});");
    m_sqlDeleteDevice = cfg.getValue("database","delete_device",
	"DELETE FROM iot_devices WHERE device_id='${device}';");
    m_sqlSelectDevice = cfg.getValue("database","select_device",
	"SELECT token_hash,name,enabled,last_seen FROM iot_devices WHERE device_id='${device}' LIMIT 1;");
    m_sqlUpdateLastSeen = cfg.getValue("database","update_last_seen",
	"UPDATE iot_devices SET last_seen=${now}, updated_at=${now} WHERE device_id='${device}';");
    m_sqlInsertEvent = cfg.getValue("database","insert_event",
	"INSERT INTO iot_events(device_id,ts,kind,proto,payload)"
	" VALUES('${device}',${ts},'${kind}','${proto}','${payload}');");
    m_sqlListDevices = cfg.getValue("database","list_devices",
	"SELECT device_id,name,enabled,last_seen FROM iot_devices ORDER BY device_id LIMIT ${limit} OFFSET ${offset};");
    m_sqlListDevicesCount = cfg.getValue("database","list_devices_count",
	"SELECT COUNT(*) FROM iot_devices;");
    m_defaultListLimit = cfg.getIntValue("general","default_list_limit",10000,100,1000000,false);
    m_maxListLimit = cfg.getIntValue("general","max_list_limit",50000,100,1000000,false);
    if (m_maxListLimit < m_defaultListLimit)
	m_maxListLimit = m_defaultListLimit;

    if (m_autoCreateTables)
	dbInit();

    m_hAuth = new IotAuthHandler();
    m_hUplink = new IotUplinkHandler();
    m_hDevCreate = new IotDeviceCreateHandler();
    m_hDevDelete = new IotDeviceDeleteHandler();
    m_hDevGet = new IotDeviceGetHandler();
    m_hDevList = new IotDeviceListHandler();

    Engine::install(m_hAuth);
    Engine::install(m_hUplink);
    Engine::install(m_hDevCreate);
    Engine::install(m_hDevDelete);
    Engine::install(m_hDevGet);
    Engine::install(m_hDevList);
}

bool IotDevModule::unload()
{
    if (!lock(500000))
	return false;
    Engine::uninstall(m_hAuth);
    Engine::uninstall(m_hUplink);
    Engine::uninstall(m_hDevCreate);
    Engine::uninstall(m_hDevDelete);
    Engine::uninstall(m_hDevGet);
    Engine::uninstall(m_hDevList);
    unlock();
    return true;
}

bool IotDevModule::dbDispatch(Message& db)
{
    bool ok = Engine::dispatch(db) && !db.getParam("error");
    const char* e = db.getValue("error");
    if (!ok && !TelEngine::null(e))
	Debug(this,DebugWarn,"DB error: %s",e);
    return ok;
}

bool IotDevModule::dbExec(const String& queryTmpl, const NamedList& params, bool results, Message& out)
{
    if (queryTmpl.null())
	return false;
    out.clearParams();
    out.retValue().clear();
    out.addParam("module",name());
    out.addParam("account",m_accountDB);
    String q = queryTmpl;
    params.replaceParams(q,true);
    out.addParam("query",q);
    out.addParam("results",String::boolText(results));
    return dbDispatch(out);
}

bool IotDevModule::dbInit()
{
    NamedList p("");
    Message out("database");
    bool ok1 = dbExec(m_sqlCreateDevices,p,false,out);
    bool ok2 = dbExec(m_sqlCreateEvents,p,false,out);
    return ok1 && ok2;
}

bool IotDevModule::dbGetDevice(const String& deviceId, String& tokenHash, String& name, int& enabled, int64_t& lastSeen)
{
    NamedList p("");
    p.addParam("device",sqlEscape(deviceId));
    Message db("database");
    if (!dbExec(m_sqlSelectDevice,p,true,db))
	return false;
    if (db.getIntValue("rows") < 1)
	return false;

    Array* a = static_cast<Array*>(db.userObject(YATOM("Array")));
    if (!a)
	return false;

    String* th = YOBJECT(String,a->get(0,1));
    String* nm = YOBJECT(String,a->get(1,1));
    String* en = YOBJECT(String,a->get(2,1));
    String* ls = YOBJECT(String,a->get(3,1));
    if (!th)
	return false;
    tokenHash = *th;
    if (nm)
	name = *nm;
    enabled = en ? en->toInteger(1) : 1;
    lastSeen = ls ? ls->toInt64(0) : 0;
    return true;
}

bool IotDevModule::dbUpdateLastSeen(const String& deviceId, int64_t nowSec)
{
    NamedList p("");
    p.addParam("device",sqlEscape(deviceId));
    p.addParam("now",String(nowSec));
    Message out("database");
    return dbExec(m_sqlUpdateLastSeen,p,false,out);
}

bool IotDevModule::dbInsertEvent(const String& deviceId, const String& kind, const String& proto, int64_t tsSec, const String& payload)
{
    NamedList p("");
    p.addParam("device",sqlEscape(deviceId));
    p.addParam("kind",sqlEscape(kind));
    p.addParam("proto",sqlEscape(proto));
    p.addParam("ts",String(tsSec));
    p.addParam("payload",sqlEscape(payload));
    Message out("database");
    return dbExec(m_sqlInsertEvent,p,false,out);
}

bool IotDevModule::authDevice(const String& deviceId, const String& token, bool updateLastSeen, String* err)
{
    if (deviceId.null()) {
	if (err) *err = "missing device";
	return false;
    }
    if (token.null()) {
	if (err) *err = "missing token";
	return false;
    }
    String tokenHash;
    String devName;
    int enabled = 1;
    int64_t lastSeen = 0;
    if (!dbGetDevice(deviceId,tokenHash,devName,enabled,lastSeen)) {
	if (err) *err = "device not found";
	return false;
    }
    if (!enabled) {
	if (err) *err = "device disabled";
	return false;
    }
    SHA256 sha(token);
    String got = sha.hexDigest();
    if (got.null() || tokenHash.null() || got != tokenHash) {
	if (err) *err = "invalid token";
	return false;
    }
    if (updateLastSeen) {
	int64_t nowSec = (int64_t)Time::secNow();
	dbUpdateLastSeen(deviceId,nowSec);
    }
    return true;
}

bool IotDevModule::handleAuth(Message& msg)
{
    const String* dev = msg.getParam("device");
    const String* tok = msg.getParam("token");
    if (TelEngine::null(dev) || TelEngine::null(tok)) {
	msg.setParam("error","missing device/token");
	return true;
    }
    String err;
    bool ok = authDevice(*dev,*tok,true,&err);
    if (!ok) {
	msg.setParam("error",err);
	msg.retValue() = "false";
	return true;
    }
    msg.setParam("authed","true");
    msg.retValue() = "true";
    return true;
}

bool IotDevModule::handleUplink(Message& msg)
{
    const String* dev = msg.getParam("device");
    if (TelEngine::null(dev)) {
	msg.setParam("error","missing device");
	return true;
    }

    String proto = msg.getValue("proto","unknown");
    String kind = msg.getValue("kind",msg.getValue("type","telemetry"));
    String payload = msg.getValue("payload");

    if (payload.length() > (unsigned int)m_maxPayload)
	payload = payload.substr(0,m_maxPayload);

    bool authed = msg.getBoolValue("authed",false);
    if (!authed && !m_allowUnauth) {
	const String* tok = msg.getParam("token");
	String err;
	if (!tok || !authDevice(*dev,*tok,true,&err)) {
	    msg.setParam("error",err.null() ? "unauthorized" : err);
	    msg.retValue() = "unauthorized";
	    return true;
	}
    }
    else if (authed) {
	int64_t nowSec = (int64_t)Time::secNow();
	dbUpdateLastSeen(*dev,nowSec);
    }

    int64_t nowSec = (int64_t)Time::secNow();
    int64_t tsSec = parseTs(msg.getValue("ts"),nowSec);
    if (!dbInsertEvent(*dev,kind,proto,tsSec,payload)) {
	msg.setParam("error","db insert failed");
	msg.retValue() = "error";
	return true;
    }
    msg.retValue() = "ok";
    return true;
}

bool IotDevModule::handleDeviceCreate(Message& msg)
{
    const String* dev = msg.getParam("device");
    const String* tok = msg.getParam("token");
    if (TelEngine::null(dev) || TelEngine::null(tok)) {
	msg.setParam("error","missing device/token");
	return true;
    }
    String nameVal = msg.getValue("name");
    int64_t nowSec = (int64_t)Time::secNow();
    SHA256 sha(*tok);
    String tokenHash = sha.hexDigest();

    NamedList p("");
    p.addParam("device",sqlEscape(*dev));
    p.addParam("token_hash",sqlEscape(tokenHash));
    p.addParam("name",sqlEscape(nameVal));
    p.addParam("now",String(nowSec));

    Message out("database");
    if (!dbExec(m_sqlInsertDevice,p,false,out)) {
	msg.setParam("error","db insert failed");
	msg.retValue() = "error";
	return true;
    }
    msg.retValue() = "created";
    return true;
}

bool IotDevModule::handleDeviceDelete(Message& msg)
{
    const String* dev = msg.getParam("device");
    if (TelEngine::null(dev)) {
	msg.setParam("error","missing device");
	return true;
    }
    NamedList p("");
    p.addParam("device",sqlEscape(*dev));
    Message out("database");
    if (!dbExec(m_sqlDeleteDevice,p,false,out)) {
	msg.setParam("error","db delete failed");
	msg.retValue() = "error";
	return true;
    }
    msg.retValue() = "deleted";
    return true;
}

bool IotDevModule::handleDeviceGet(Message& msg)
{
    const String* dev = msg.getParam("device");
    if (TelEngine::null(dev)) {
	msg.setParam("error","missing device");
	return true;
    }
    String tokenHash;
    String name;
    int enabled = 1;
    int64_t lastSeen = 0;
    if (!dbGetDevice(*dev,tokenHash,name,enabled,lastSeen)) {
	msg.setParam("error","not found");
	msg.retValue() = "not_found";
	return true;
    }
    msg.retValue().clear();
    msg.retValue() << "device=" << *dev << "\r\n";
    msg.retValue() << "name=" << name << "\r\n";
    msg.retValue() << "enabled=" << enabled << "\r\n";
    msg.retValue() << "last_seen=" << (int64_t)lastSeen << "\r\n";
    msg.retValue() << "token_hash=" << tokenHash << "\r\n";
    return true;
}

bool IotDevModule::handleDeviceList(Message& msg)
{
    int limit = msg.getIntValue("limit",m_defaultListLimit,1,m_maxListLimit,false);
    int offset = msg.getIntValue("offset",0,0,10000000,false);

    // Total count (for pagination)
    if (!m_sqlListDevicesCount.null()) {
	NamedList pc("");
	Message dbc("database");
	if (dbExec(m_sqlListDevicesCount,pc,true,dbc) && dbc.getIntValue("rows") >= 1) {
	    Array* ac = static_cast<Array*>(dbc.userObject(YATOM("Array")));
	    if (ac) {
		String* cnt = YOBJECT(String,ac->get(0,1));
		if (cnt)
		    msg.setParam("total",*cnt);
	    }
	}
    }

    NamedList p("");
    p.addParam("limit",String(limit));
    p.addParam("offset",String(offset));
    Message db("database");
    if (!dbExec(m_sqlListDevices,p,true,db)) {
	msg.setParam("error","db query failed");
	msg.retValue() = "error";
	return true;
    }
    Array* a = static_cast<Array*>(db.userObject(YATOM("Array")));
    if (!a || db.getIntValue("rows") < 1) {
	msg.retValue() = "device_id\tname\tenabled\tlast_seen\r\n";
	return true;
    }
    msg.retValue().clear();
    msg.retValue() << "device_id\tname\tenabled\tlast_seen\r\n";
    int rows = db.getIntValue("rows");
    for (int r = 1; r <= rows; r++) {
	String* did = YOBJECT(String,a->get(0,r));
	String* nm = YOBJECT(String,a->get(1,r));
	String* en = YOBJECT(String,a->get(2,r));
	String* ls = YOBJECT(String,a->get(3,r));
	if (!did)
	    continue;
	msg.retValue() << *did << "\t" << (nm ? nm->c_str() : "") << "\t"
	    << (en ? en->c_str() : "1") << "\t" << (ls ? ls->c_str() : "0") << "\r\n";
    }
    return true;
}

bool IotAuthHandler::received(Message& msg)
{
    return __plugin.handleAuth(msg);
}

bool IotUplinkHandler::received(Message& msg)
{
    return __plugin.handleUplink(msg);
}

bool IotDeviceCreateHandler::received(Message& msg)
{
    return __plugin.handleDeviceCreate(msg);
}

bool IotDeviceDeleteHandler::received(Message& msg)
{
    return __plugin.handleDeviceDelete(msg);
}

bool IotDeviceGetHandler::received(Message& msg)
{
    return __plugin.handleDeviceGet(msg);
}

bool IotDeviceListHandler::received(Message& msg)
{
    return __plugin.handleDeviceList(msg);
}

} // namespace

/* vi: set ts=8 sw=4 sts=4 noet: */

