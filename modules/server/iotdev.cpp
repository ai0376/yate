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

class IotEventQueryHandler : public MessageHandler
{
public:
    inline IotEventQueryHandler(unsigned int prio = 100)
	: MessageHandler("iot.event.query",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotEventLatestHandler : public MessageHandler
{
public:
    inline IotEventLatestHandler(unsigned int prio = 100)
	: MessageHandler("iot.event.latest",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotCommandSendHandler : public MessageHandler
{
public:
    inline IotCommandSendHandler(unsigned int prio = 100)
	: MessageHandler("iot.command.send",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotCommandListHandler : public MessageHandler
{
public:
    inline IotCommandListHandler(unsigned int prio = 100)
	: MessageHandler("iot.command.list",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotCommandMarkSentHandler : public MessageHandler
{
public:
    inline IotCommandMarkSentHandler(unsigned int prio = 100)
	: MessageHandler("iot.command.mark_sent",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotCommandAckHandler : public MessageHandler
{
public:
    inline IotCommandAckHandler(unsigned int prio = 100)
	: MessageHandler("iot.command.ack",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotRuleCreateHandler : public MessageHandler
{
public:
    inline IotRuleCreateHandler(unsigned int prio = 100)
	: MessageHandler("iot.rule.create",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotRuleListHandler : public MessageHandler
{
public:
    inline IotRuleListHandler(unsigned int prio = 100)
	: MessageHandler("iot.rule.list",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotRuleDeleteHandler : public MessageHandler
{
public:
    inline IotRuleDeleteHandler(unsigned int prio = 100)
	: MessageHandler("iot.rule.delete",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotAlarmCreateHandler : public MessageHandler
{
public:
    inline IotAlarmCreateHandler(unsigned int prio = 100)
	: MessageHandler("iot.alarm.create",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotAlarmListHandler : public MessageHandler
{
public:
    inline IotAlarmListHandler(unsigned int prio = 100)
	: MessageHandler("iot.alarm.list",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotAlarmAckHandler : public MessageHandler
{
public:
    inline IotAlarmAckHandler(unsigned int prio = 100)
	: MessageHandler("iot.alarm.ack",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotApikeyValidateHandler : public MessageHandler
{
public:
    inline IotApikeyValidateHandler(unsigned int prio = 100)
	: MessageHandler("iot.apikey.validate",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotApikeyCreateHandler : public MessageHandler
{
public:
    inline IotApikeyCreateHandler(unsigned int prio = 100)
	: MessageHandler("iot.apikey.create",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotApikeyListHandler : public MessageHandler
{
public:
    inline IotApikeyListHandler(unsigned int prio = 100)
	: MessageHandler("iot.apikey.list",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotApikeyDeleteHandler : public MessageHandler
{
public:
    inline IotApikeyDeleteHandler(unsigned int prio = 100)
	: MessageHandler("iot.apikey.delete",prio,0)
	{ }
    virtual bool received(Message& msg);
};

class IotAuditLogHandler : public MessageHandler
{
public:
    inline IotAuditLogHandler(unsigned int prio = 100)
	: MessageHandler("iot.audit.log",prio,0)
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
    bool handleEventQuery(Message& msg);
    bool handleEventLatest(Message& msg);
    bool handleCommandSend(Message& msg);
    bool handleCommandList(Message& msg);
    bool handleCommandMarkSent(Message& msg);
    bool handleCommandAck(Message& msg);
    bool handleRuleCreate(Message& msg);
    bool handleRuleList(Message& msg);
    bool handleRuleDelete(Message& msg);
    bool handleAlarmCreate(Message& msg);
    bool handleAlarmList(Message& msg);
    bool handleAlarmAck(Message& msg);
    bool handleApikeyValidate(Message& msg);
    bool handleApikeyCreate(Message& msg);
    bool handleApikeyList(Message& msg);
    bool handleApikeyDelete(Message& msg);
    bool handleAuditLog(Message& msg);

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

    // Event query
    String m_sqlListEvents;
    String m_sqlListEventsCount;
    String m_sqlSelectLatest;
    int m_defaultEventLimit;
    int m_maxEventLimit;

    // Commands
    String m_sqlCreateCommands;
    String m_sqlInsertCommand;
    String m_sqlListCommands;
    String m_sqlMarkCommandSent;
    String m_sqlAckCommand;

    // Rules and alarms
    String m_sqlCreateRules;
    String m_sqlCreateAlarms;
    String m_sqlInsertRule;
    String m_sqlListRules;
    String m_sqlDeleteRule;
    String m_sqlInsertAlarm;
    String m_sqlListAlarms;
    String m_sqlAckAlarm;
    int m_defaultAlarmLimit;
    int m_maxAlarmLimit;

    // API keys and audit
    String m_sqlCreateApikeys;
    String m_sqlCreateAuditLog;
    String m_sqlSelectApikeyByHash;
    String m_sqlInsertApikey;
    String m_sqlListApikeys;
    String m_sqlDeleteApikey;
    String m_sqlInsertAudit;

    // Handlers
    IotAuthHandler* m_hAuth;
    IotUplinkHandler* m_hUplink;
    IotDeviceCreateHandler* m_hDevCreate;
    IotDeviceDeleteHandler* m_hDevDelete;
    IotDeviceGetHandler* m_hDevGet;
    IotDeviceListHandler* m_hDevList;
    IotEventQueryHandler* m_hEventQuery;
    IotEventLatestHandler* m_hEventLatest;
    IotCommandSendHandler* m_hCommandSend;
    IotCommandListHandler* m_hCommandList;
    IotCommandMarkSentHandler* m_hCommandMarkSent;
    IotCommandAckHandler* m_hCommandAck;
    IotRuleCreateHandler* m_hRuleCreate;
    IotRuleListHandler* m_hRuleList;
    IotRuleDeleteHandler* m_hRuleDelete;
    IotAlarmCreateHandler* m_hAlarmCreate;
    IotAlarmListHandler* m_hAlarmList;
    IotAlarmAckHandler* m_hAlarmAck;
    IotApikeyValidateHandler* m_hApikeyValidate;
    IotApikeyCreateHandler* m_hApikeyCreate;
    IotApikeyListHandler* m_hApikeyList;
    IotApikeyDeleteHandler* m_hApikeyDelete;
    IotAuditLogHandler* m_hAuditLog;
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
      m_defaultEventLimit(1000),
      m_maxEventLimit(10000),
      m_hAuth(0),
      m_hUplink(0),
      m_hDevCreate(0),
      m_hDevDelete(0),
      m_hDevGet(0),
      m_hDevList(0),
      m_hEventQuery(0),
      m_hEventLatest(0),
      m_hCommandSend(0),
      m_hCommandList(0),
      m_hCommandMarkSent(0),
      m_hCommandAck(0),
      m_hRuleCreate(0),
      m_hRuleList(0),
      m_hRuleDelete(0),
      m_hAlarmCreate(0),
      m_hAlarmList(0),
      m_hAlarmAck(0),
      m_hApikeyValidate(0),
      m_hApikeyCreate(0),
      m_hApikeyList(0),
      m_hApikeyDelete(0),
      m_hAuditLog(0)
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
    TelEngine::destruct(m_hEventQuery);
    TelEngine::destruct(m_hEventLatest);
    TelEngine::destruct(m_hCommandSend);
    TelEngine::destruct(m_hCommandList);
    TelEngine::destruct(m_hCommandMarkSent);
    TelEngine::destruct(m_hCommandAck);
    TelEngine::destruct(m_hRuleCreate);
    TelEngine::destruct(m_hRuleList);
    TelEngine::destruct(m_hRuleDelete);
    TelEngine::destruct(m_hAlarmCreate);
    TelEngine::destruct(m_hAlarmList);
    TelEngine::destruct(m_hAlarmAck);
    TelEngine::destruct(m_hApikeyValidate);
    TelEngine::destruct(m_hApikeyCreate);
    TelEngine::destruct(m_hApikeyList);
    TelEngine::destruct(m_hApikeyDelete);
    TelEngine::destruct(m_hAuditLog);
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

    m_sqlListEvents = cfg.getValue("database","list_events",
	"SELECT id,device_id,ts,kind,proto,payload FROM iot_events WHERE device_id='${device}' AND ts>=${from_ts} AND ts<=${to_ts} AND ${kind_cond} ORDER BY ts DESC LIMIT ${limit} OFFSET ${offset};");
    m_sqlListEventsCount = cfg.getValue("database","list_events_count",
	"SELECT COUNT(*) FROM iot_events WHERE device_id='${device}' AND ts>=${from_ts} AND ts<=${to_ts} AND ${kind_cond};");
    m_sqlSelectLatest = cfg.getValue("database","select_latest",
	"SELECT id,device_id,ts,kind,proto,payload FROM iot_events WHERE device_id='${device}' AND ${kind_cond} ORDER BY ts DESC LIMIT ${limit};");
    m_defaultEventLimit = cfg.getIntValue("general","default_event_limit",1000,1,100000,false);
    m_maxEventLimit = cfg.getIntValue("general","max_event_limit",10000,1,100000,false);
    if (m_maxEventLimit < m_defaultEventLimit)
	m_maxEventLimit = m_defaultEventLimit;

    m_sqlCreateCommands = cfg.getValue("database","create_commands",
	"CREATE TABLE IF NOT EXISTS iot_commands ("
	" command_id TEXT PRIMARY KEY,"
	" device_id TEXT NOT NULL,"
	" payload TEXT NOT NULL,"
	" status TEXT NOT NULL DEFAULT 'pending',"
	" created_ts INTEGER NOT NULL,"
	" sent_ts INTEGER,"
	" ack_ts INTEGER,"
	" ack_payload TEXT"
	");"
	"CREATE INDEX IF NOT EXISTS idx_iot_commands_device_status ON iot_commands(device_id,status);");
    m_sqlInsertCommand = cfg.getValue("database","insert_command",
	"INSERT INTO iot_commands(command_id,device_id,payload,status,created_ts)"
	" VALUES('${command_id}','${device}','${payload}','pending',${now});");
    m_sqlListCommands = cfg.getValue("database","list_commands",
	"SELECT command_id,device_id,payload,status,created_ts FROM iot_commands"
	" WHERE device_id='${device}' AND status='${status}' ORDER BY created_ts ASC LIMIT ${limit};");
    m_sqlMarkCommandSent = cfg.getValue("database","mark_command_sent",
	"UPDATE iot_commands SET status='sent', sent_ts=${now} WHERE command_id='${command_id}';");
    m_sqlAckCommand = cfg.getValue("database","ack_command",
	"UPDATE iot_commands SET status='acked', ack_ts=${now}, ack_payload='${ack_payload}' WHERE command_id='${command_id}';");

    m_sqlCreateRules = cfg.getValue("database","create_rules",
	"CREATE TABLE IF NOT EXISTS iot_rules ("
	" rule_id TEXT PRIMARY KEY,"
	" name TEXT,"
	" device_id TEXT NOT NULL,"
	" kind TEXT NOT NULL,"
	" key_name TEXT NOT NULL,"
	" op TEXT NOT NULL,"
	" value TEXT NOT NULL,"
	" webhook_url TEXT,"
	" alarm_level TEXT NOT NULL DEFAULT 'warning',"
	" enabled INTEGER NOT NULL DEFAULT 1"
	");");
    m_sqlCreateAlarms = cfg.getValue("database","create_alarms",
	"CREATE TABLE IF NOT EXISTS iot_alarms ("
	" id INTEGER PRIMARY KEY AUTOINCREMENT,"
	" device_id TEXT NOT NULL,"
	" rule_id TEXT NOT NULL,"
	" start_ts INTEGER NOT NULL,"
	" end_ts INTEGER,"
	" level TEXT NOT NULL,"
	" acked INTEGER NOT NULL DEFAULT 0,"
	" ack_ts INTEGER,"
	" payload_snapshot TEXT,"
	" created_ts INTEGER NOT NULL"
	");"
	"CREATE INDEX IF NOT EXISTS idx_iot_alarms_device ON iot_alarms(device_id);"
	"CREATE INDEX IF NOT EXISTS idx_iot_alarms_rule ON iot_alarms(rule_id);");
    m_sqlInsertRule = cfg.getValue("database","insert_rule",
	"INSERT INTO iot_rules(rule_id,name,device_id,kind,key_name,op,value,webhook_url,alarm_level,enabled)"
	" VALUES('${rule_id}','${name}','${device_id}','${kind}','${key_name}','${op}','${value}','${webhook_url}','${alarm_level}',${enabled});");
    m_sqlListRules = cfg.getValue("database","list_rules",
	"SELECT rule_id,name,device_id,kind,key_name,op,value,webhook_url,alarm_level FROM iot_rules"
	" WHERE (device_id='*' OR device_id='${device_id}') AND kind='${kind}' AND enabled=1 ORDER BY rule_id;");
    m_sqlDeleteRule = cfg.getValue("database","delete_rule",
	"DELETE FROM iot_rules WHERE rule_id='${rule_id}';");
    m_sqlInsertAlarm = cfg.getValue("database","insert_alarm",
	"INSERT INTO iot_alarms(device_id,rule_id,start_ts,level,payload_snapshot,created_ts)"
	" VALUES('${device_id}','${rule_id}',${start_ts},'${level}','${payload_snapshot}',${created_ts});");
    m_sqlListAlarms = cfg.getValue("database","list_alarms",
	"SELECT id,device_id,rule_id,start_ts,end_ts,level,acked,ack_ts,payload_snapshot,created_ts FROM iot_alarms"
	" WHERE device_id='${device_id}' AND (${active_filter}) ORDER BY start_ts DESC LIMIT ${limit} OFFSET ${offset};");
    m_sqlAckAlarm = cfg.getValue("database","ack_alarm",
	"UPDATE iot_alarms SET acked=1, ack_ts=${now} WHERE id=${alarm_id};");
    m_defaultAlarmLimit = cfg.getIntValue("general","default_alarm_limit",500,1,10000,false);
    m_maxAlarmLimit = cfg.getIntValue("general","max_alarm_limit",5000,1,100000,false);
    if (m_maxAlarmLimit < m_defaultAlarmLimit)
	m_maxAlarmLimit = m_defaultAlarmLimit;

    m_sqlCreateApikeys = cfg.getValue("database","create_apikeys",
	"CREATE TABLE IF NOT EXISTS iot_api_keys ("
	" key_id TEXT PRIMARY KEY,"
	" key_hash TEXT NOT NULL,"
	" name TEXT,"
	" role TEXT NOT NULL DEFAULT 'operator',"
	" enabled INTEGER NOT NULL DEFAULT 1,"
	" created_ts INTEGER NOT NULL"
	");");
    m_sqlCreateAuditLog = cfg.getValue("database","create_audit_log",
	"CREATE TABLE IF NOT EXISTS iot_audit_log ("
	" id INTEGER PRIMARY KEY AUTOINCREMENT,"
	" actor_type TEXT NOT NULL,"
	" actor_id TEXT NOT NULL,"
	" action TEXT NOT NULL,"
	" target_id TEXT,"
	" ts INTEGER NOT NULL,"
	" result TEXT NOT NULL,"
	" details TEXT"
	");"
	"CREATE INDEX IF NOT EXISTS idx_iot_audit_ts ON iot_audit_log(ts);");
    m_sqlSelectApikeyByHash = cfg.getValue("database","select_apikey_by_hash",
	"SELECT key_id,name,role FROM iot_api_keys WHERE key_hash='${key_hash}' AND enabled=1 LIMIT 1;");
    m_sqlInsertApikey = cfg.getValue("database","insert_apikey",
	"INSERT INTO iot_api_keys(key_id,key_hash,name,role,enabled,created_ts)"
	" VALUES('${key_id}','${key_hash}','${name}','${role}',1,${now});");
    m_sqlListApikeys = cfg.getValue("database","list_apikeys",
	"SELECT key_id,name,role,enabled,created_ts FROM iot_api_keys ORDER BY created_ts;");
    m_sqlDeleteApikey = cfg.getValue("database","delete_apikey",
	"DELETE FROM iot_api_keys WHERE key_id='${key_id}';");
    m_sqlInsertAudit = cfg.getValue("database","insert_audit",
	"INSERT INTO iot_audit_log(actor_type,actor_id,action,target_id,ts,result,details)"
	" VALUES('${actor_type}','${actor_id}','${action}','${target_id}',${ts},'${result}','${details}');");

    if (m_autoCreateTables)
	dbInit();

    m_hAuth = new IotAuthHandler();
    m_hUplink = new IotUplinkHandler();
    m_hDevCreate = new IotDeviceCreateHandler();
    m_hDevDelete = new IotDeviceDeleteHandler();
    m_hDevGet = new IotDeviceGetHandler();
    m_hDevList = new IotDeviceListHandler();
    m_hEventQuery = new IotEventQueryHandler();
    m_hEventLatest = new IotEventLatestHandler();
    m_hCommandSend = new IotCommandSendHandler();
    m_hCommandList = new IotCommandListHandler();
    m_hCommandMarkSent = new IotCommandMarkSentHandler();
    m_hCommandAck = new IotCommandAckHandler();
    m_hRuleCreate = new IotRuleCreateHandler();
    m_hRuleList = new IotRuleListHandler();
    m_hRuleDelete = new IotRuleDeleteHandler();
    m_hAlarmCreate = new IotAlarmCreateHandler();
    m_hAlarmList = new IotAlarmListHandler();
    m_hAlarmAck = new IotAlarmAckHandler();
    m_hApikeyValidate = new IotApikeyValidateHandler();
    m_hApikeyCreate = new IotApikeyCreateHandler();
    m_hApikeyList = new IotApikeyListHandler();
    m_hApikeyDelete = new IotApikeyDeleteHandler();
    m_hAuditLog = new IotAuditLogHandler();

    Engine::install(m_hAuth);
    Engine::install(m_hUplink);
    Engine::install(m_hDevCreate);
    Engine::install(m_hDevDelete);
    Engine::install(m_hDevGet);
    Engine::install(m_hDevList);
    Engine::install(m_hEventQuery);
    Engine::install(m_hEventLatest);
    Engine::install(m_hCommandSend);
    Engine::install(m_hCommandList);
    Engine::install(m_hCommandMarkSent);
    Engine::install(m_hCommandAck);
    Engine::install(m_hRuleCreate);
    Engine::install(m_hRuleList);
    Engine::install(m_hRuleDelete);
    Engine::install(m_hAlarmCreate);
    Engine::install(m_hAlarmList);
    Engine::install(m_hAlarmAck);
    Engine::install(m_hApikeyValidate);
    Engine::install(m_hApikeyCreate);
    Engine::install(m_hApikeyList);
    Engine::install(m_hApikeyDelete);
    Engine::install(m_hAuditLog);
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
    Engine::uninstall(m_hEventQuery);
    Engine::uninstall(m_hEventLatest);
    Engine::uninstall(m_hCommandSend);
    Engine::uninstall(m_hCommandList);
    Engine::uninstall(m_hCommandMarkSent);
    Engine::uninstall(m_hCommandAck);
    Engine::uninstall(m_hRuleCreate);
    Engine::uninstall(m_hRuleList);
    Engine::uninstall(m_hRuleDelete);
    Engine::uninstall(m_hAlarmCreate);
    Engine::uninstall(m_hAlarmList);
    Engine::uninstall(m_hAlarmAck);
    Engine::uninstall(m_hApikeyValidate);
    Engine::uninstall(m_hApikeyCreate);
    Engine::uninstall(m_hApikeyList);
    Engine::uninstall(m_hApikeyDelete);
    Engine::uninstall(m_hAuditLog);
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
    bool ok3 = dbExec(m_sqlCreateCommands,p,false,out);
    bool ok4 = dbExec(m_sqlCreateRules,p,false,out);
    bool ok5 = dbExec(m_sqlCreateAlarms,p,false,out);
    bool ok6 = dbExec(m_sqlCreateApikeys,p,false,out);
    bool ok7 = dbExec(m_sqlCreateAuditLog,p,false,out);
    return ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok7;
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

bool IotDevModule::handleEventQuery(Message& msg)
{
    const String* dev = msg.getParam("device");
    if (TelEngine::null(dev)) {
	msg.setParam("error","missing device");
	return true;
    }
    int64_t fromTs = msg.getInt64Value("from_ts",0,0,0,true);
    int64_t toTs = msg.getInt64Value("to_ts",0,0,0,true);
    if (toTs <= 0)
	toTs = 2147483647;
    String kind = msg.getValue("kind");
    String kindCond("1=1");
    if (kind) {
	kindCond = "kind='";
	kindCond += sqlEscape(kind);
	kindCond += "'";
    }
    int limit = msg.getIntValue("limit",m_defaultEventLimit,1,m_maxEventLimit,false);
    int offset = msg.getIntValue("offset",0,0,10000000,false);

    if (!m_sqlListEventsCount.null()) {
	NamedList pc("");
	pc.addParam("device",sqlEscape(*dev));
	pc.addParam("from_ts",String(fromTs));
	pc.addParam("to_ts",String(toTs));
	pc.addParam("kind_cond",kindCond);
	Message dbc("database");
	if (dbExec(m_sqlListEventsCount,pc,true,dbc) && dbc.getIntValue("rows") >= 1) {
	    Array* ac = static_cast<Array*>(dbc.userObject(YATOM("Array")));
	    if (ac) {
		String* cnt = YOBJECT(String,ac->get(0,1));
		if (cnt)
		    msg.setParam("total",*cnt);
	    }
	}
    }

    NamedList p("");
    p.addParam("device",sqlEscape(*dev));
    p.addParam("from_ts",String(fromTs));
    p.addParam("to_ts",String(toTs));
    p.addParam("kind_cond",kindCond);
    p.addParam("limit",String(limit));
    p.addParam("offset",String(offset));
    Message db("database");
    if (!dbExec(m_sqlListEvents,p,true,db)) {
	msg.setParam("error","db query failed");
	msg.retValue() = "error";
	return true;
    }
    Array* a = static_cast<Array*>(db.userObject(YATOM("Array")));
    if (!a || db.getIntValue("rows") < 1) {
	msg.retValue() = "id\tdevice_id\tts\tkind\tproto\tpayload\r\n";
	return true;
    }
    msg.retValue().clear();
    msg.retValue() << "id\tdevice_id\tts\tkind\tproto\tpayload\r\n";
    int rows = db.getIntValue("rows");
    for (int r = 1; r <= rows; r++) {
	String* id_ = YOBJECT(String,a->get(0,r));
	String* did = YOBJECT(String,a->get(1,r));
	String* ts_ = YOBJECT(String,a->get(2,r));
	String* k = YOBJECT(String,a->get(3,r));
	String* proto = YOBJECT(String,a->get(4,r));
	String* pl = YOBJECT(String,a->get(5,r));
	if (!id_)
	    continue;
	msg.retValue() << *id_ << "\t" << (did ? did->c_str() : "") << "\t"
	    << (ts_ ? ts_->c_str() : "0") << "\t" << (k ? k->c_str() : "") << "\t"
	    << (proto ? proto->c_str() : "") << "\t" << (pl ? pl->c_str() : "") << "\r\n";
    }
    return true;
}

bool IotDevModule::handleEventLatest(Message& msg)
{
    const String* dev = msg.getParam("device");
    if (TelEngine::null(dev)) {
	msg.setParam("error","missing device");
	return true;
    }
    String kind = msg.getValue("kind");
    String kindCond("1=1");
    if (kind) {
	kindCond = "kind='";
	kindCond += sqlEscape(kind);
	kindCond += "'";
    }
    int limit = msg.getIntValue("limit",kind ? 1 : 10,1,m_maxEventLimit,false);

    NamedList p("");
    p.addParam("device",sqlEscape(*dev));
    p.addParam("kind_cond",kindCond);
    p.addParam("limit",String(limit));
    Message db("database");
    if (!dbExec(m_sqlSelectLatest,p,true,db)) {
	msg.setParam("error","db query failed");
	msg.retValue() = "error";
	return true;
    }
    Array* a = static_cast<Array*>(db.userObject(YATOM("Array")));
    if (!a || db.getIntValue("rows") < 1) {
	msg.retValue() = "id\tdevice_id\tts\tkind\tproto\tpayload\r\n";
	return true;
    }
    msg.retValue().clear();
    msg.retValue() << "id\tdevice_id\tts\tkind\tproto\tpayload\r\n";
    int rows = db.getIntValue("rows");
    for (int r = 1; r <= rows; r++) {
	String* id_ = YOBJECT(String,a->get(0,r));
	String* did = YOBJECT(String,a->get(1,r));
	String* ts_ = YOBJECT(String,a->get(2,r));
	String* k = YOBJECT(String,a->get(3,r));
	String* proto = YOBJECT(String,a->get(4,r));
	String* pl = YOBJECT(String,a->get(5,r));
	if (!id_)
	    continue;
	msg.retValue() << *id_ << "\t" << (did ? did->c_str() : "") << "\t"
	    << (ts_ ? ts_->c_str() : "0") << "\t" << (k ? k->c_str() : "") << "\t"
	    << (proto ? proto->c_str() : "") << "\t" << (pl ? pl->c_str() : "") << "\r\n";
    }
    return true;
}

bool IotDevModule::handleCommandSend(Message& msg)
{
    const String* dev = msg.getParam("device");
    const String* payload = msg.getParam("payload");
    if (TelEngine::null(dev) || TelEngine::null(payload)) {
	msg.setParam("error","missing device or payload");
	return true;
    }
    String commandId = msg.getValue("command_id");
    if (commandId.null()) {
	commandId << (int64_t)Time::msecNow() << "_" << (unsigned int)Random::random() % 1000000u;
    }
    int64_t nowSec = (int64_t)Time::secNow();
    NamedList p("");
    p.addParam("command_id",sqlEscape(commandId));
    p.addParam("device",sqlEscape(*dev));
    p.addParam("payload",sqlEscape(*payload));
    p.addParam("now",String(nowSec));
    Message out("database");
    if (!dbExec(m_sqlInsertCommand,p,false,out)) {
	msg.setParam("error","db insert failed");
	msg.retValue() = "error";
	return true;
    }
    msg.setParam("command_id",commandId);
    msg.retValue() = commandId;
    return true;
}

bool IotDevModule::handleCommandList(Message& msg)
{
    const String* dev = msg.getParam("device");
    if (TelEngine::null(dev)) {
	msg.setParam("error","missing device");
	return true;
    }
    String status = msg.getValue("status","pending");
    int limit = msg.getIntValue("limit",50,1,500,false);
    NamedList p("");
    p.addParam("device",sqlEscape(*dev));
    p.addParam("status",sqlEscape(status));
    p.addParam("limit",String(limit));
    Message db("database");
    if (!dbExec(m_sqlListCommands,p,true,db)) {
	msg.setParam("error","db query failed");
	msg.retValue() = "error";
	return true;
    }
    Array* a = static_cast<Array*>(db.userObject(YATOM("Array")));
    if (!a || db.getIntValue("rows") < 1) {
	msg.retValue() = "command_id\tdevice_id\tpayload\tstatus\tcreated_ts\r\n";
	return true;
    }
    msg.retValue().clear();
    msg.retValue() << "command_id\tdevice_id\tpayload\tstatus\tcreated_ts\r\n";
    int rows = db.getIntValue("rows");
    for (int r = 1; r <= rows; r++) {
	String* cid = YOBJECT(String,a->get(0,r));
	String* did = YOBJECT(String,a->get(1,r));
	String* pl = YOBJECT(String,a->get(2,r));
	String* st = YOBJECT(String,a->get(3,r));
	String* ct = YOBJECT(String,a->get(4,r));
	if (!cid)
	    continue;
	msg.retValue() << *cid << "\t" << (did ? did->c_str() : "") << "\t"
	    << (pl ? pl->c_str() : "") << "\t" << (st ? st->c_str() : "") << "\t"
	    << (ct ? ct->c_str() : "0") << "\r\n";
    }
    return true;
}

bool IotDevModule::handleCommandMarkSent(Message& msg)
{
    const String* cid = msg.getParam("command_id");
    if (TelEngine::null(cid)) {
	msg.setParam("error","missing command_id");
	return true;
    }
    int64_t nowSec = (int64_t)Time::secNow();
    NamedList p("");
    p.addParam("command_id",sqlEscape(*cid));
    p.addParam("now",String(nowSec));
    Message out("database");
    if (!dbExec(m_sqlMarkCommandSent,p,false,out)) {
	msg.setParam("error","db update failed");
	msg.retValue() = "error";
	return true;
    }
    msg.retValue() = "ok";
    return true;
}

bool IotDevModule::handleCommandAck(Message& msg)
{
    const String* cid = msg.getParam("command_id");
    if (TelEngine::null(cid)) {
	msg.setParam("error","missing command_id");
	return true;
    }
    String ackPayload = msg.getValue("ack_payload");
    int64_t nowSec = (int64_t)Time::secNow();
    NamedList p("");
    p.addParam("command_id",sqlEscape(*cid));
    p.addParam("ack_payload",sqlEscape(ackPayload));
    p.addParam("now",String(nowSec));
    Message out("database");
    if (!dbExec(m_sqlAckCommand,p,false,out)) {
	msg.setParam("error","db update failed");
	msg.retValue() = "error";
	return true;
    }
    msg.retValue() = "ok";
    return true;
}

bool IotDevModule::handleRuleCreate(Message& msg)
{
    const String* rid = msg.getParam("rule_id");
    const String* dev = msg.getParam("device_id");
    const String* kind = msg.getParam("kind");
    const String* keyName = msg.getParam("key_name");
    const String* op = msg.getParam("op");
    const String* value = msg.getParam("value");
    if (TelEngine::null(rid) || TelEngine::null(dev) || TelEngine::null(kind) || TelEngine::null(keyName) || TelEngine::null(op) || TelEngine::null(value)) {
	msg.setParam("error","missing rule_id/device_id/kind/key_name/op/value");
	return true;
    }
    String name = msg.getValue("name");
    String webhookUrl = msg.getValue("webhook_url");
    String alarmLevel = msg.getValue("alarm_level","warning");
    int enabled = msg.getBoolValue("enabled",true) ? 1 : 0;
    NamedList p("");
    p.addParam("rule_id",sqlEscape(*rid));
    p.addParam("name",sqlEscape(name));
    p.addParam("device_id",sqlEscape(*dev));
    p.addParam("kind",sqlEscape(*kind));
    p.addParam("key_name",sqlEscape(*keyName));
    p.addParam("op",sqlEscape(*op));
    p.addParam("value",sqlEscape(*value));
    p.addParam("webhook_url",sqlEscape(webhookUrl));
    p.addParam("alarm_level",sqlEscape(alarmLevel));
    p.addParam("enabled",String(enabled));
    Message out("database");
    if (!dbExec(m_sqlInsertRule,p,false,out)) {
	msg.setParam("error","db insert failed");
	msg.retValue() = "error";
	return true;
    }
    msg.retValue() = "created";
    return true;
}

bool IotDevModule::handleRuleList(Message& msg)
{
    const String* dev = msg.getParam("device_id");
    const String* kind = msg.getParam("kind");
    if (TelEngine::null(dev) || TelEngine::null(kind)) {
	msg.setParam("error","missing device_id or kind");
	return true;
    }
    NamedList p("");
    p.addParam("device_id",sqlEscape(*dev));
    p.addParam("kind",sqlEscape(*kind));
    Message db("database");
    if (!dbExec(m_sqlListRules,p,true,db)) {
	msg.setParam("error","db query failed");
	msg.retValue() = "error";
	return true;
    }
    Array* a = static_cast<Array*>(db.userObject(YATOM("Array")));
    if (!a || db.getIntValue("rows") < 1) {
	msg.retValue() = "rule_id\tname\tdevice_id\tkind\tkey_name\top\tvalue\twebhook_url\talarm_level\r\n";
	return true;
    }
    msg.retValue().clear();
    msg.retValue() << "rule_id\tname\tdevice_id\tkind\tkey_name\top\tvalue\twebhook_url\talarm_level\r\n";
    int rows = db.getIntValue("rows");
    for (int r = 1; r <= rows; r++) {
	String* rid = YOBJECT(String,a->get(0,r));
	String* nm = YOBJECT(String,a->get(1,r));
	String* did = YOBJECT(String,a->get(2,r));
	String* k = YOBJECT(String,a->get(3,r));
	String* kn = YOBJECT(String,a->get(4,r));
	String* o = YOBJECT(String,a->get(5,r));
	String* v = YOBJECT(String,a->get(6,r));
	String* wu = YOBJECT(String,a->get(7,r));
	String* al = YOBJECT(String,a->get(8,r));
	if (!rid)
	    continue;
	msg.retValue() << *rid << "\t" << (nm ? nm->c_str() : "") << "\t" << (did ? did->c_str() : "") << "\t"
	    << (k ? k->c_str() : "") << "\t" << (kn ? kn->c_str() : "") << "\t" << (o ? o->c_str() : "") << "\t"
	    << (v ? v->c_str() : "") << "\t" << (wu ? wu->c_str() : "") << "\t" << (al ? al->c_str() : "warning") << "\r\n";
    }
    return true;
}

bool IotDevModule::handleRuleDelete(Message& msg)
{
    const String* rid = msg.getParam("rule_id");
    if (TelEngine::null(rid)) {
	msg.setParam("error","missing rule_id");
	return true;
    }
    NamedList p("");
    p.addParam("rule_id",sqlEscape(*rid));
    Message out("database");
    if (!dbExec(m_sqlDeleteRule,p,false,out)) {
	msg.setParam("error","db delete failed");
	msg.retValue() = "error";
	return true;
    }
    msg.retValue() = "deleted";
    return true;
}

bool IotDevModule::handleAlarmCreate(Message& msg)
{
    const String* dev = msg.getParam("device_id");
    const String* rid = msg.getParam("rule_id");
    const String* level = msg.getParam("level");
    if (TelEngine::null(dev) || TelEngine::null(rid)) {
	msg.setParam("error","missing device_id or rule_id");
	return true;
    }
    int64_t nowSec = (int64_t)Time::secNow();
    String payloadSnap = msg.getValue("payload_snapshot");
    if (payloadSnap.length() > (unsigned int)m_maxPayload)
	payloadSnap = payloadSnap.substr(0,m_maxPayload);
    String levelVal = level && *level ? *level : "warning";
    NamedList p("");
    p.addParam("device_id",sqlEscape(*dev));
    p.addParam("rule_id",sqlEscape(*rid));
    p.addParam("start_ts",String(nowSec));
    p.addParam("level",sqlEscape(levelVal));
    p.addParam("payload_snapshot",sqlEscape(payloadSnap));
    p.addParam("created_ts",String(nowSec));
    Message out("database");
    if (!dbExec(m_sqlInsertAlarm,p,false,out)) {
	msg.setParam("error","db insert failed");
	msg.retValue() = "error";
	return true;
    }
    msg.retValue() = "created";
    return true;
}

bool IotDevModule::handleAlarmList(Message& msg)
{
    const String* dev = msg.getParam("device_id");
    if (TelEngine::null(dev)) {
	msg.setParam("error","missing device_id");
	return true;
    }
    bool activeOnly = msg.getBoolValue("active_only",false);
    String activeFilter = activeOnly ? "end_ts IS NULL" : "1=1";
    int limit = msg.getIntValue("limit",m_defaultAlarmLimit,1,m_maxAlarmLimit,false);
    int offset = msg.getIntValue("offset",0,0,10000000,false);
    NamedList p("");
    p.addParam("device_id",sqlEscape(*dev));
    p.addParam("active_filter",activeFilter);
    p.addParam("limit",String(limit));
    p.addParam("offset",String(offset));
    Message db("database");
    if (!dbExec(m_sqlListAlarms,p,true,db)) {
	msg.setParam("error","db query failed");
	msg.retValue() = "error";
	return true;
    }
    Array* a = static_cast<Array*>(db.userObject(YATOM("Array")));
    if (!a || db.getIntValue("rows") < 1) {
	msg.retValue() = "id\tdevice_id\trule_id\tstart_ts\tend_ts\tlevel\tacked\tack_ts\tpayload_snapshot\tcreated_ts\r\n";
	return true;
    }
    msg.retValue().clear();
    msg.retValue() << "id\tdevice_id\trule_id\tstart_ts\tend_ts\tlevel\tacked\tack_ts\tpayload_snapshot\tcreated_ts\r\n";
    int rows = db.getIntValue("rows");
    for (int r = 1; r <= rows; r++) {
	String* id_ = YOBJECT(String,a->get(0,r));
	String* did = YOBJECT(String,a->get(1,r));
	String* rid = YOBJECT(String,a->get(2,r));
	String* st = YOBJECT(String,a->get(3,r));
	String* et = YOBJECT(String,a->get(4,r));
	String* lv = YOBJECT(String,a->get(5,r));
	String* acked = YOBJECT(String,a->get(6,r));
	String* at = YOBJECT(String,a->get(7,r));
	String* pl = YOBJECT(String,a->get(8,r));
	String* ct = YOBJECT(String,a->get(9,r));
	if (!id_)
	    continue;
	msg.retValue() << *id_ << "\t" << (did ? did->c_str() : "") << "\t" << (rid ? rid->c_str() : "") << "\t"
	    << (st ? st->c_str() : "0") << "\t" << (et ? et->c_str() : "") << "\t" << (lv ? lv->c_str() : "") << "\t"
	    << (acked ? acked->c_str() : "0") << "\t" << (at ? at->c_str() : "") << "\t" << (pl ? pl->c_str() : "") << "\t"
	    << (ct ? ct->c_str() : "0") << "\r\n";
    }
    return true;
}

bool IotDevModule::handleAlarmAck(Message& msg)
{
    const String* aid = msg.getParam("alarm_id");
    if (TelEngine::null(aid) || aid->null()) {
	msg.setParam("error","missing alarm_id");
	return true;
    }
    int64_t idVal = aid->toInt64(-1);
    if (idVal < 1) {
	msg.setParam("error","invalid alarm_id");
	return true;
    }
    int64_t nowSec = (int64_t)Time::secNow();
    NamedList p("");
    p.addParam("alarm_id",String(idVal));
    p.addParam("now",String(nowSec));
    Message out("database");
    if (!dbExec(m_sqlAckAlarm,p,false,out)) {
	msg.setParam("error","db update failed");
	msg.retValue() = "error";
	return true;
    }
    msg.retValue() = "ok";
    return true;
}

bool IotDevModule::handleApikeyValidate(Message& msg)
{
    const String* key = msg.getParam("api_key");
    if (TelEngine::null(key) || key->null()) {
	msg.setParam("error","missing api_key");
	return true;
    }
    SHA256 sha(*key);
    String keyHash = sha.hexDigest();
    NamedList p("");
    p.addParam("key_hash",sqlEscape(keyHash));
    Message db("database");
    if (!dbExec(m_sqlSelectApikeyByHash,p,true,db) || db.getIntValue("rows") < 1) {
	msg.setParam("error","invalid or disabled api_key");
	msg.retValue() = "invalid";
	return true;
    }
    Array* a = static_cast<Array*>(db.userObject(YATOM("Array")));
    if (!a)
	return true;
    String* kid = YOBJECT(String,a->get(0,1));
    String* nm = YOBJECT(String,a->get(1,1));
    String* role = YOBJECT(String,a->get(2,1));
    if (kid)
	msg.setParam("key_id",*kid);
    if (nm)
	msg.setParam("name",*nm);
    msg.setParam("role",role && *role ? role->c_str() : "operator");
    msg.retValue() = "ok";
    return true;
}

bool IotDevModule::handleApikeyCreate(Message& msg)
{
    const String* kid = msg.getParam("key_id");
    const String* key = msg.getParam("api_key");
    const String* name = msg.getParam("name");
    const String* role = msg.getParam("role");
    if (TelEngine::null(kid) || kid->null() || TelEngine::null(key) || key->null()) {
	msg.setParam("error","missing key_id or api_key");
	return true;
    }
    String roleVal = (role && *role) ? *role : "operator";
    if (roleVal != "admin" && roleVal != "operator" && roleVal != "readonly")
	roleVal = "operator";
    SHA256 sha(*key);
    String keyHash = sha.hexDigest();
    int64_t nowSec = (int64_t)Time::secNow();
    NamedList p("");
    p.addParam("key_id",sqlEscape(*kid));
    p.addParam("key_hash",sqlEscape(keyHash));
    p.addParam("name",sqlEscape(name ? *name : ""));
    p.addParam("role",sqlEscape(roleVal));
    p.addParam("now",String(nowSec));
    Message out("database");
    if (!dbExec(m_sqlInsertApikey,p,false,out)) {
	msg.setParam("error","db insert failed");
	msg.retValue() = "error";
	return true;
    }
    msg.setParam("key_id",*kid);
    msg.retValue() = "created";
    return true;
}

bool IotDevModule::handleApikeyList(Message& msg)
{
    NamedList p("");
    Message db("database");
    if (!dbExec(m_sqlListApikeys,p,true,db)) {
	msg.setParam("error","db query failed");
	msg.retValue() = "error";
	return true;
    }
    Array* a = static_cast<Array*>(db.userObject(YATOM("Array")));
    if (!a || db.getIntValue("rows") < 1) {
	msg.retValue() = "key_id\tname\trole\tenabled\tcreated_ts\r\n";
	return true;
    }
    msg.retValue().clear();
    msg.retValue() << "key_id\tname\trole\tenabled\tcreated_ts\r\n";
    int rows = db.getIntValue("rows");
    for (int r = 1; r <= rows; r++) {
	String* kid = YOBJECT(String,a->get(0,r));
	String* nm = YOBJECT(String,a->get(1,r));
	String* role = YOBJECT(String,a->get(2,r));
	String* en = YOBJECT(String,a->get(3,r));
	String* ct = YOBJECT(String,a->get(4,r));
	if (!kid)
	    continue;
	msg.retValue() << *kid << "\t" << (nm ? nm->c_str() : "") << "\t" << (role ? role->c_str() : "operator") << "\t"
	    << (en ? en->c_str() : "1") << "\t" << (ct ? ct->c_str() : "0") << "\r\n";
    }
    return true;
}

bool IotDevModule::handleApikeyDelete(Message& msg)
{
    const String* kid = msg.getParam("key_id");
    if (TelEngine::null(kid)) {
	msg.setParam("error","missing key_id");
	return true;
    }
    NamedList p("");
    p.addParam("key_id",sqlEscape(*kid));
    Message out("database");
    if (!dbExec(m_sqlDeleteApikey,p,false,out)) {
	msg.setParam("error","db delete failed");
	msg.retValue() = "error";
	return true;
    }
    msg.retValue() = "deleted";
    return true;
}

bool IotDevModule::handleAuditLog(Message& msg)
{
    const String* actorType = msg.getParam("actor_type");
    const String* actorId = msg.getParam("actor_id");
    const String* action = msg.getParam("action");
    const String* result = msg.getParam("result");
    if (TelEngine::null(actorType) || TelEngine::null(actorId) || TelEngine::null(action) || TelEngine::null(result)) {
	return true;
    }
    String targetId = msg.getValue("target_id");
    String details = msg.getValue("details");
    if (details.length() > (unsigned int)m_maxPayload)
	details = details.substr(0,m_maxPayload);
    int64_t nowSec = (int64_t)Time::secNow();
    NamedList p("");
    p.addParam("actor_type",sqlEscape(*actorType));
    p.addParam("actor_id",sqlEscape(*actorId));
    p.addParam("action",sqlEscape(*action));
    p.addParam("target_id",sqlEscape(targetId));
    p.addParam("ts",String(nowSec));
    p.addParam("result",sqlEscape(*result));
    p.addParam("details",sqlEscape(details));
    Message out("database");
    dbExec(m_sqlInsertAudit,p,false,out);
    msg.retValue() = "ok";
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

bool IotEventQueryHandler::received(Message& msg)
{
    return __plugin.handleEventQuery(msg);
}

bool IotEventLatestHandler::received(Message& msg)
{
    return __plugin.handleEventLatest(msg);
}

bool IotCommandSendHandler::received(Message& msg)
{
    return __plugin.handleCommandSend(msg);
}

bool IotCommandListHandler::received(Message& msg)
{
    return __plugin.handleCommandList(msg);
}

bool IotCommandMarkSentHandler::received(Message& msg)
{
    return __plugin.handleCommandMarkSent(msg);
}

bool IotCommandAckHandler::received(Message& msg)
{
    return __plugin.handleCommandAck(msg);
}

bool IotRuleCreateHandler::received(Message& msg)
{
    return __plugin.handleRuleCreate(msg);
}

bool IotRuleListHandler::received(Message& msg)
{
    return __plugin.handleRuleList(msg);
}

bool IotRuleDeleteHandler::received(Message& msg)
{
    return __plugin.handleRuleDelete(msg);
}

bool IotAlarmCreateHandler::received(Message& msg)
{
    return __plugin.handleAlarmCreate(msg);
}

bool IotAlarmListHandler::received(Message& msg)
{
    return __plugin.handleAlarmList(msg);
}

bool IotAlarmAckHandler::received(Message& msg)
{
    return __plugin.handleAlarmAck(msg);
}

bool IotApikeyValidateHandler::received(Message& msg)
{
    return __plugin.handleApikeyValidate(msg);
}

bool IotApikeyCreateHandler::received(Message& msg)
{
    return __plugin.handleApikeyCreate(msg);
}

bool IotApikeyListHandler::received(Message& msg)
{
    return __plugin.handleApikeyList(msg);
}

bool IotApikeyDeleteHandler::received(Message& msg)
{
    return __plugin.handleApikeyDelete(msg);
}

bool IotAuditLogHandler::received(Message& msg)
{
    return __plugin.handleAuditLog(msg);
}

} // namespace

/* vi: set ts=8 sw=4 sts=4 noet: */

