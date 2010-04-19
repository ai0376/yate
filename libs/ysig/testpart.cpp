/**
 * testpart.cpp
 * This file is part of the YATE Project http://YATE.null.ro 
 *
 * Yet Another Signalling Stack - implements the support for SS7, ISDN and PSTN
 *
 * Yet Another Telephony Engine - a fully featured software PBX and IVR
 * Copyright (C) 2010 Null Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "yatesig.h"
#include <yatephone.h>


using namespace TelEngine;

#define CMD_STOP   0
#define CMD_SINGLE 1
#define CMD_START  2

// Control operations
static const TokenDict s_dict_control[] = {
    { "stop", CMD_STOP, },
    { "single", CMD_SINGLE, },
    { "start", CMD_START, },
    { 0, 0 }
};

bool SS7Testing::receivedMSU(const SS7MSU& msu, const SS7Label& label, SS7Layer3* network, int sls)
{
    if (msu.getSIF() != SS7MSU::MTP_T)
	return false;
    XDebug(this,DebugStub,"Possibly incomplete SS7Testing::receivedMSU(%p,%p,%p,%d) [%p]",
	&msu,&label,network,sls,this);

    // Q.782 2.3: 4 bytes message number, 2 bytes length (9 bits used), N bytes zeros
    const unsigned char* s = msu.getData(label,6);
    if (!s)
	return false;
    u_int32_t seq = s[0] + ((u_int32_t)s[1] << 8) +
	((u_int32_t)s[2] << 16) + ((u_int32_t)s[3] << 24);
    u_int16_t len = s[4] + ((u_int16_t)s[5] << 8);

    const unsigned char* t = msu.getData(label.length()+6,len);
    if (!t) {
	Debug(this,DebugMild,"Received MTP_T seq %u, length %u with invalid test length %u [%p]",
	    seq,msu.length(),len,this);
	return false;
    }
    Debug(this,DebugNote,"Received MTP_T seq %u, test length %u",seq,len);
    return true;
}

bool SS7Testing::sendTraffic()
{
    if (!m_lbl.length())
	return false;
    u_int32_t seq = m_seq++;
    u_int16_t len = m_len + 6;
    SS7MSU msu(SS7MSU::MTP_T,m_lbl,0,len);
    unsigned char* d = msu.getData(m_lbl,len);
    if (!d)
	return false;
    for (unsigned int i = 0; i < 4; i++)
	*d++ = 0xff & (seq << (8 * i));
    *d++ = m_len & 0xff;
    *d++ = (m_len >> 8) & 0xff;
    Debug(this,DebugInfo,"Sending MTP_T seq %u, test length %u",seq,m_len);
    return transmitMSU(msu,m_lbl,m_lbl.sls()) >= 0;
}

void SS7Testing::notify(SS7Layer3* network, int sls)
{
    Debug(this,DebugStub,"Please implement SS7Testing::notify(%p,%d) [%p]",
	network,sls,this);
}

void SS7Testing::timerTick(const Time& when)
{
    Lock mylock(this);
    if (!m_timer.timeout(when.msec()))
	return;
    m_timer.start(when.msec());
    sendTraffic();
}

bool SS7Testing::initialize(const NamedList* config)
{
    if (!config)
	return true;
    Lock mylock(this);
    setParams(*config);
    if (config->getBoolValue("autostart",false)) {
	if (m_timer.interval() && m_lbl.length())
	    m_timer.start();
	sendTraffic();
    }
    return true;
}

bool SS7Testing::control(NamedList& params)
{
    String* ret = params.getParam("completion");
    const String* oper = params.getParam("operation");
    const char* cmp = params.getValue("component");
    int cmd = oper ? oper->toInteger(s_dict_control,-1) : -1;

    if (ret) {
	if (oper && (cmd < 0))
	    return false;
	String part = params.getValue("partword");
	if (cmp) {
	    if (toString() != cmp)
		return false;
	    for (const TokenDict* d = s_dict_control; d->token; d++)
		Module::itemComplete(*ret,d->token,part);
	    return true;
	}
	return Module::itemComplete(*ret,toString(),part);
    }

    if (!(cmp && toString() == cmp))
	return false;
    if (cmd >= 0) {
	Lock mylock(this);
	setParams(params,true);
	switch (cmd) {
	    case CMD_STOP:
		m_timer.stop();
		return true;
	    case CMD_START:
		if (!(m_timer.interval() && m_lbl.length()))
		    return false;
		m_timer.start();
		return sendTraffic();
	    case CMD_SINGLE:
		m_timer.stop();
		return sendTraffic();
	}
    }
    return SignallingComponent::control(params);
}

void SS7Testing::setParams(const NamedList& params, bool setSeq)
{
    m_timer.interval(params,"interval",20,500,true);
    m_len = params.getIntValue("length",m_len);
    if (m_len > 1024)
	m_len = 1024;
    if (setSeq || !m_seq)
	m_seq = params.getIntValue("sequence",m_seq);
    const String* lbl = params.getParam("address");
    if (!TelEngine::null(lbl)) {
	// TYPE,opc,dpc,sls,spare
	SS7PointCode::Type t = SS7PointCode::Other;
	ObjList* l = lbl->split(',');
	const GenObject* o = l->at(0);
	if (o) {
	    t = SS7PointCode::lookup(o->toString());
	    if (t == SS7PointCode::Other)
		t = m_lbl.type();
	}
	if (t != SS7PointCode::Other) {
	    o = l->at(1);
	    if (o) {
		SS7PointCode c(m_lbl.opc());
		if (c.assign(o->toString(),t))
		    m_lbl.assign(t,m_lbl.dpc(),c,m_lbl.sls(),m_lbl.spare());
	    }
	    o = l->at(2);
	    if (o) {
		SS7PointCode c(m_lbl.dpc());
		if (c.assign(o->toString(),t))
		    m_lbl.assign(t,c,m_lbl.opc(),m_lbl.sls(),m_lbl.spare());
	    }
	    o = l->at(3);
	    if (o) {
		int sls = o->toString().toInteger(-1);
		if (sls >= 0)
		    m_lbl.setSls(sls);
	    }
	    o = l->at(4);
	    if (o) {
		int spare = o->toString().toInteger(-1);
		if (spare >= 0)
		    m_lbl.setSpare(spare);
	    }
	}
	delete l;
    }
}

/* vi: set ts=8 sw=4 sts=4 noet: */