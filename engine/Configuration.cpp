/**
 * Configuration.cpp
 * This file is part of the YATE Project http://YATE.null.ro
 *
 * Yet Another Telephony Engine - a fully featured software PBX and IVR
 * Copyright (C) 2004-2020 Null Team
 *
 * This software is distributed under multiple licenses;
 * see the COPYING file in the main directory for licensing
 * information for this specific distribution.
 *
 * This use of this software may be subject to additional restrictions.
 * See the LEGAL file in the main directory for details.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 */

#include "yatengine.h"

#include <stdio.h>
#include <string.h>

using namespace TelEngine;

static unsigned int s_maxDepth = 3;
static int s_disableIncludeSilent = -1;

class ConfigurationPrivate
{
public:
    enum Include {
	IncludeNone = 0,
	Include = 1,
	IncludeSilent = 2,
	IncludeRequire = 3,
    };
    inline ConfigurationPrivate(Configuration& cfg, bool isMain)
	: m_cfg(cfg), m_main(isMain)
	{}
    inline void addingParam(const String& sect, const String& name, const String& value) {
	    if (!m_main || sect != YSTRING("configuration"))
		return;
	    if (s_maxDepthInit && name == YSTRING("max_depth")) {
		s_maxDepthInit = false;
		s_maxDepth = value.toInteger(3,0,3,10);
	    }
	    else if (s_disableIncludeSilent < 0 && name == YSTRING("disable_include_silent"))
		s_disableIncludeSilent = value.toBoolean() ? 1 : 0;
	}
    inline bool prepareIncludeSection(const String& sect, String& s, const char* file, bool warn,
	bool& ok) {
	    int inc = getIncludeSect(s);
	    if (!inc)
		return false;
	    NamedList* nl = sect ? m_cfg.getSection(sect) : 0;
	    if (nl) {
		nl->addParam("[]",s);
		if (!m_includeSections.find(nl))
		    m_includeSections.append(nl)->setDelete(false);
		XDebug(DebugAll,"Config '%s' prepared section '%s' include '%s' file='%s'",
		    m_cfg.safe(),sect.safe(),s.safe(),(file == m_cfg.c_str() ? "<same>" : file));
	    }
	    else {
		if (inc == IncludeRequire)
		    ok = false;
		if (getWarn(warn,inc == IncludeSilent)) {
		    String tmp;
		    if (file != m_cfg.c_str())
			tmp.printf(" in included file '%s'",file);
		    Debug(DebugNote,"Config '%s' found '%s' outside any section%s",
			m_cfg.safe(),s.safe(),tmp.safe());
		}
	    }
	    return true;
	}
    inline void processIncludeSections(bool warn, bool& ok) {
	    for (ObjList* o = m_includeSections.skipNull(); o; o = o->skipNext()) {
		ObjList stack;
		processInclude(static_cast<NamedList*>(o->get()),stack,warn,ok);
	    }
	}
    inline bool getWarn(bool warn, bool silent)
	{ return (warn && silent) ? (s_disableIncludeSilent > 0) : warn; }

    static inline int getIncludeSect(String& buf, bool setName = false) {
	    if (buf.startsWith("$includesection",true))
		{ if (setName) buf = buf.substr(16,buf.length() - 16); return Include; }
	    if (buf.startsWith("$includesectionsilent",true))
		{ if (setName) buf = buf.substr(22,buf.length() - 22); return IncludeSilent; }
	    if (buf.startsWith("$requiresection",true))
		{ if (setName) buf = buf.substr(16,buf.length() - 16); return IncludeRequire; }
	    return 0;
	}

    static bool s_maxDepthInit;

private:
    void processInclude(NamedList* sect, ObjList& stack, bool warn, bool& ok);

    Configuration& m_cfg;
    bool m_main;
    ObjList m_includeSections;
    ObjList m_includeSectProcessed;
};
bool ConfigurationPrivate::s_maxDepthInit = true;

void ConfigurationPrivate::processInclude(NamedList* sect, ObjList& stack, bool warn, bool& ok)
{
    if (!sect || m_includeSectProcessed.find(sect))
	return;
    stack.append(sect)->setDelete(false);
#ifdef XDEBUG
    String tmp;
    tmp.append(stack," -> ");
    Debug(DebugInfo,"Config '%s' processing include section stack: %s",
	m_cfg.safe(),tmp.safe());
#endif
    for (ObjList* o = sect->paramList()->skipNull(); o;) {
	NamedString* s = static_cast<NamedString*>(o->get());
	int inc = 0;
	if ('[' == s->name()[0] && ']' == s->name()[1])
	    inc = getIncludeSect(*s,true);
	if (!inc) {
	    o = o->skipNext();
	    continue;
	}
	Engine::runParams().replaceParams(*s);
	if (*s) {
	    String error;
	    if (!stack[*s]) {
		// NOTE: We are adding current section to processed after processing it
		//       Handle already processed sections whithout checking for recursive include
		NamedList* incSect = static_cast<NamedList*>(m_includeSectProcessed[*s]);
		if (!incSect) {
		    incSect = m_cfg.getSection(*s);
		    if (incSect && incSect != sect)
			processInclude(incSect,stack,warn,ok);
		    else
			error = incSect ? "recursive include" : "not found";
		}
		if (!error) {
		    XDebug(DebugAll,"Config '%s' including section '%s' in '%s'",
			m_cfg.safe(),incSect->safe(),sect->safe());
		    for (ObjList* p = incSect->paramList()->skipNull(); p; p = p->skipNext()) {
			NamedString* ns = static_cast<NamedString*>(p->get());
			o->insert(new NamedString(ns->name(),*ns));
			// Update current element (replaced by insert)
			o = o->next();
		    }
		}
	    }
	    else {
		error.append(stack," -> ");
		error = "recursive include stack=" + error;
	    }
	    if (error) {
		if (inc == IncludeRequire)
		    ok = false;
		if (getWarn(warn,inc == IncludeSilent))
		    Debug(DebugNote,"Config '%s' not including section '%s' in '%s': %s",
			m_cfg.safe(),s->safe(),sect->safe(),error.c_str());
	    }
	}
	o->remove();
	o = o->skipNull();
	if (o)
	    continue;
	sect->paramList()->compact();
	break;
    }
    stack.remove(sect,false);
    m_includeSectProcessed.insert(sect)->setDelete(false);
}


// Text sort callback
static int textSort(GenObject* obj1, GenObject* obj2, void* context)
{
    const String* s1 = static_cast<const String*>(obj1);
    const String* s2 = static_cast<const String*>(obj2);
    if (TelEngine::null(s1))
	return TelEngine::null(s2) ? 0 : -1;
    if (TelEngine::null(s2))
	return 1;
    return ::strcmp(s1->c_str(),s2->c_str());
}


Configuration::Configuration()
    : m_main(false)
{
}

Configuration::Configuration(const char* filename, bool warn)
    : String(filename), m_main(false)
{
    load(warn);
}

ObjList* Configuration::getSectHolder(const String& sect) const
{
    if (sect.null())
	return 0;
    return const_cast<ObjList*>(m_sections.find(sect));
}

ObjList* Configuration::makeSectHolder(const String& sect)
{
    if (sect.null())
	return 0;
    ObjList *l = getSectHolder(sect);
    if (!l)
	l = m_sections.append(new NamedList(sect));
    return l;
}

NamedList* Configuration::getSection(unsigned int index) const
{
    return static_cast<NamedList *>(m_sections[index]);
}

NamedList* Configuration::getSection(const String& sect) const
{
    ObjList *l = getSectHolder(sect);
    return l ? static_cast<NamedList *>(l->get()) : 0;
}

NamedString* Configuration::getKey(const String& sect, const String& key) const
{
    NamedList *l = getSection(sect);
    return l ? l->getParam(key) : 0;
}

const char* Configuration::getValue(const String& sect, const String& key, const char* defvalue) const
{
    const NamedString *s = getKey(sect,key);
    return s ? s->c_str() : defvalue;
}

int Configuration::getIntValue(const String& sect, const String& key, int defvalue,
    int minvalue, int maxvalue, bool clamp) const
{
    const NamedString *s = getKey(sect,key);
    return s ? s->toInteger(defvalue,0,minvalue,maxvalue,clamp) : defvalue;
}

int Configuration::getIntValue(const String& sect, const String& key, const TokenDict* tokens, int defvalue) const
{
    const NamedString *s = getKey(sect,key);
    return s ? s->toInteger(tokens,defvalue) : defvalue;
}

int64_t Configuration::getInt64Value(const String& sect, const String& key, int64_t defvalue,
    int64_t minvalue, int64_t maxvalue, bool clamp) const
{
    const NamedString *s = getKey(sect,key);
    return s ? s->toInt64(defvalue,0,minvalue,maxvalue,clamp) : defvalue;
}

double Configuration::getDoubleValue(const String& sect, const String& key, double defvalue) const
{
    const NamedString *s = getKey(sect,key);
    return s ? s->toDouble(defvalue) : defvalue;
}

bool Configuration::getBoolValue(const String& sect, const String& key, bool defvalue) const
{
    const NamedString *s = getKey(sect,key);
    return s ? s->toBoolean(defvalue) : defvalue;
}

void Configuration::clearSection(const char* sect)
{
    if (sect) {
	ObjList *l = getSectHolder(sect);
	if (l)
	    l->remove();
    }
    else
	m_sections.clear();
}

// Make sure a section with a given name exists, create it if required
NamedList* Configuration::createSection(const String& sect)
{
    ObjList* o = makeSectHolder(sect);
    return o ? static_cast<NamedList*>(o->get()) : 0;
}

void Configuration::clearKey(const String& sect, const String& key)
{
    NamedList *l = getSection(sect);
    if (l)
	l->clearParam(key);
}

void Configuration::addValue(const String& sect, const char* key, const char* value)
{
    DDebug(DebugAll,"Configuration::addValue(\"%s\",\"%s\",\"%s\")",sect.c_str(),key,value);
    ObjList *l = makeSectHolder(sect);
    if (!l)
	return;
    NamedList *n = static_cast<NamedList *>(l->get());
    if (n)
	n->addParam(key,value);
}

void Configuration::setValue(const String& sect, const char* key, const char* value)
{
    DDebug(DebugAll,"Configuration::setValue(\"%s\",\"%s\",\"%s\")",sect.c_str(),key,value);
    ObjList *l = makeSectHolder(sect);
    if (!l)
	return;
    NamedList *n = static_cast<NamedList *>(l->get());
    if (n)
	n->setParam(key,value);
}

void Configuration::setValue(const String& sect, const char* key, int value)
{
    char buf[32];
    ::sprintf(buf,"%d",value);
    setValue(sect,key,buf);
}

void Configuration::setValue(const String& sect, const char* key, bool value)
{
    setValue(sect,key,String::boolText(value));
}

bool Configuration::load(bool warn)
{
    m_sections.clear();
    if (null())
	return false;
    ConfigurationPrivate priv(*this,m_main);
    return loadFile(c_str(),"",0,warn,&priv);
}

static inline char* cfgReadLine(FILE* f, char* buf, int rd,
    char& rest, bool& warn, const char* file, const String& sect, bool* start = 0)
{
    if (rest) {
	buf[0] = rest;
	rest = 0;
	buf[1] = 0;
	fgets(buf + 1,rd - 1,f);
    }
    else if (!::fgets(buf,rd,f))
	return 0;

    int check = warn ? 1 : 0;
    char* pc = ::strchr(buf,'\r');
    if (pc) {
	*pc = 0;
	check = 0;
    }
    pc = ::strchr(buf,'\n');
    if (pc) {
	*pc = 0;
	check = 0;
    }
    pc = buf;
    if (check)
	check = ::strlen(pc);
    // skip over an initial UTF-8 BOM
    if (start && *start) {
	String::stripBOM(pc);
	*start = false;
    }
    if (check == rd - 1) {
	char extra[2] = {0,0};
	::fgets(extra,2,f);
	rest = extra[0];
	if (rest) {
	    warn = false;
	    String tmp(pc);
	    if (sect)
		tmp.printf("section='%s' line %s...",sect.c_str(),tmp.substr(0,30).c_str());
	    else
		tmp.printf("line %s...",tmp.substr(0,30).c_str());
	    Debug(DebugWarn,
		"Configuration '%s' %s too long: subsequent read may lead to wrong parameter set",
		file,tmp.safe());
	}
    }
    while (*pc == ' ' || *pc == '\t')
	pc++;
    return pc;
}

bool Configuration::loadFile(const char* file, String sect, unsigned int depth, bool warn, void* priv)
{
    ConfigurationPrivate& cfg = *(ConfigurationPrivate*)priv;
    DDebug(DebugInfo,"Configuration::loadFile(\"%s\",[%s],%u,%s)",
	file,sect.c_str(),depth,String::boolText(warn));
    if (depth > s_maxDepth) {
	Debug(DebugWarn,"Config '%s' refusing to load config file '%s' at include depth %u",
	    c_str(),file,depth);
	return false;
    }
    FILE *f = ::fopen(file,"r");
    if (f) {
	bool ok = true;
	bool start = true;
	bool enabled = true;
	char rest = 0;
	bool warnLine = true;
	for (;;) {
	    char buf[1024];
	    char* pc = cfgReadLine(f,buf,sizeof(buf),rest,warnLine,file,sect,&start);
	    if (!pc)
		break;
	    switch (*pc) {
		case 0:
		case ';':
		    continue;
	    }
	    String s(pc);
	    if (s[0] == '[') {
		int r = s.find(']');
		if (r > 0) {
		    s = s.substr(1,r-1);
		    s.trimBlanks();
		    if (s.null())
			continue;
		    if (s.startSkip("$enabled")) {
			if ((s == YSTRING("else")) || (s == YSTRING("toggle")))
			    enabled = !enabled;
			else {
			    if (s.startSkip("elseif") && enabled) {
				enabled = false;
				continue;
			    }
			    Engine::runParams().replaceParams(s);
			    bool rev = s.startSkip("$not");
			    if (s.startSkip("$loaded"))
				enabled = Engine::self() && Engine::self()->pluginLoaded(s);
			    else if (s.startSkip("$unloaded"))
				enabled = !(Engine::self() && Engine::self()->pluginLoaded(s));
			    else if (s.startSkip("$filled"))
				enabled = !s.null();
			    else if (s.startSkip("$empty"))
				enabled = s.null();
			    else
				enabled = s.toBoolean(!s.startSkip("$bool"));
			    if (rev)
				enabled = !enabled;
			}
			continue;
		    }
		    if (!enabled)
			continue;
		    if (cfg.prepareIncludeSection(sect,s,file,warn,ok))
			continue;
		    bool noerr = false;
		    bool silent = false;
		    if (s.startSkip("$require") || (noerr = s.startSkip("$include"))
			|| (silent = noerr = s.startSkip("$includesilent"))) {
			Engine::runParams().replaceParams(s);
			String path;
			if (!s.startsWith(Engine::pathSeparator())) {
			    path = file;
			    int sep = path.rfind(Engine::pathSeparator());
			    if ('/' != *Engine::pathSeparator()) {
				int s2 = path.rfind('/');
				if (sep < s2)
				    sep = s2;
			    }
			    switch (sep) {
				case -1:
				    path.clear();
				    break;
				case 0:
				    path = Engine::pathSeparator();
				    break;
				default:
				    path = path.substr(0,sep);
				    path << Engine::pathSeparator();
			    }
			}
			path << s;
			ObjList files;
			bool doWarn = cfg.getWarn(warn,silent);
			if (File::listDirectory(path,0,&files)) {
			    path << Engine::pathSeparator();
			    DDebug(DebugAll,"Configuration loading up to %u files from '%s'",
				files.count(),path.c_str());
			    files.sort(textSort);
			    while (String* it = static_cast<String*>(files.remove(false))) {
				if (!(it->startsWith(".") || it->endsWith("~")
					|| it->endsWith(".bak") || it->endsWith(".tmp")))
				    ok = (loadFile(path + *it,sect,depth+1,doWarn,priv) || noerr) && ok;
#ifdef DEBUG
				else
				    Debug(DebugAll,"Configuration skipping over file '%s'",it->c_str());
#endif
				TelEngine::destruct(it);
			    }
			}
			else
			    ok = (loadFile(path,sect,depth+1,doWarn,priv) || noerr) && ok;
			continue;
		    }
		    Engine::runParams().replaceParams(s);
		    sect = s;
		    createSection(sect);
		}
		continue;
	    }
	    if (!enabled)
		continue;
	    int q = s.find('=');
	    if (q == 0)
		continue;
	    if (q < 0)
		q = s.length();
	    String key = s.substr(0,q).trimBlanks();
	    if (key.null())
		continue;
	    s = s.substr(q+1);
	    while (s.endsWith("\\",false)) {
		// line continues onto next
		s.assign(s,s.length()-1);
		char* pc = cfgReadLine(f,buf,sizeof(buf),rest,warnLine,file,sect);
		if (!pc)
		    break;
		s += pc;
	    }
	    s.trimBlanks();
	    cfg.addingParam(sect,key,s);
	    addValue(sect,key,s);
	}
	::fclose(f);
	if (!depth)
	    cfg.processIncludeSections(warn,ok);
	return ok;
    }
    if (warn) {
	int err = errno;
	if (depth)
	    Debug(DebugNote,"Config '%s' failed to open included config file '%s' (%d: %s)",
		c_str(),file,err,strerror(err));
	else
	    Debug(DebugNote,"Failed to open config file '%s', using defaults (%d: %s)",
		file,err,strerror(err));
    }
    return false;
}

bool Configuration::save() const
{
    if (null())
	return false;
    FILE *f = ::fopen(c_str(),"w");
    if (f) {
	bool separ = false;
	ObjList *ol = m_sections.skipNull();
	for (;ol;ol=ol->skipNext()) {
	    NamedList *nl = static_cast<NamedList *>(ol->get());
	    if (separ)
		::fprintf(f,"\n");
	    else
		separ = true;
	    ::fprintf(f,"[%s]\n",nl->c_str());
	    unsigned int n = nl->length();
	    for (unsigned int i = 0; i < n; i++) {
		NamedString *ns = nl->getParam(i);
		if (ns) {
		    // add a space after a line that ends with backslash
		    const char* bk = ns->endsWith("\\",false) ? " " : "";
		    ::fprintf(f,"%s=%s%s\n",ns->name().safe(),ns->safe(),bk);
		}
	    }
	}
	::fclose(f);
	return true;
    }
    int err = errno;
    Debug(DebugWarn,"Failed to save config file '%s' (%d: %s)",
	c_str(),err,strerror(err));
    return false;
}

/* vi: set ts=8 sw=4 sts=4 noet: */
