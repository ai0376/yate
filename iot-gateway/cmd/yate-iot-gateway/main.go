package main

import (
	"bufio"
	"bytes"
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"

	mqtt "github.com/mochi-mqtt/server/v2"
	"github.com/mochi-mqtt/server/v2/listeners"
	"github.com/mochi-mqtt/server/v2/packets"
	coap "github.com/plgd-dev/go-coap/v3"
	coapmux "github.com/plgd-dev/go-coap/v3/mux"
	"github.com/plgd-dev/go-coap/v3/message"
	"github.com/plgd-dev/go-coap/v3/message/codes"
)

// Yate extmodule msg escaping (compatible with TelEngine::String::msgEscape/msgUnescape).
func msgEscape(s string, extraEsc byte) string {
	if s == "" {
		return ""
	}
	var b strings.Builder
	b.Grow(len(s) + 8)
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c == '%' {
			b.WriteString("%%")
			continue
		}
		if c < ' ' || c == ':' || (extraEsc != 0 && c == extraEsc) {
			b.WriteByte('%')
			b.WriteByte(c + '@')
			continue
		}
		b.WriteByte(c)
	}
	return b.String()
}

func msgUnescape(s string) (string, error) {
	if s == "" {
		return "", nil
	}
	var b strings.Builder
	b.Grow(len(s))
	for i := 0; i < len(s); i++ {
		c := s[i]
		if c != '%' {
			if c < ' ' {
				return b.String(), fmt.Errorf("unescape: control char at %d", i)
			}
			b.WriteByte(c)
			continue
		}
		i++
		if i >= len(s) {
			return b.String(), errors.New("unescape: trailing %")
		}
		n := s[i]
		if n == '%' {
			b.WriteByte('%')
			continue
		}
		// (n > '@' && n <= '_') || n == 'z'
		if (n > '@' && n <= '_') || n == 'z' {
			b.WriteByte(n - '@')
			continue
		}
		return b.String(), fmt.Errorf("unescape: invalid escape %%%c at %d", n, i)
	}
	return b.String(), nil
}

func newMsgID() string {
	var buf [12]byte
	_, _ = rand.Read(buf[:])
	return hex.EncodeToString(buf[:])
}

type yateMsg struct {
	ID        string
	Processed bool
	Name      string
	Ret       string
	Params    map[string]string
}

type yateClient struct {
	conn net.Conn
	r    *bufio.Reader
	w    *bufio.Writer

	mu      sync.Mutex
	waiting map[string]chan yateMsg
	closed  chan struct{}
}

func dialYate(addr string) (*yateClient, error) {
	c, err := net.Dial("tcp", addr)
	if err != nil {
		return nil, err
	}
	yc := &yateClient{
		conn:    c,
		r:       bufio.NewReaderSize(c, 64*1024),
		w:       bufio.NewWriterSize(c, 64*1024),
		waiting: map[string]chan yateMsg{},
		closed:  make(chan struct{}),
	}
	return yc, nil
}

func (c *yateClient) close() {
	c.mu.Lock()
	select {
	case <-c.closed:
		c.mu.Unlock()
		return
	default:
		close(c.closed)
	}
	_ = c.conn.Close()
	for _, ch := range c.waiting {
		close(ch)
	}
	c.waiting = map[string]chan yateMsg{}
	c.mu.Unlock()
}

func (c *yateClient) sendLine(line string) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	_, err := c.w.WriteString(line + "\n")
	if err != nil {
		return err
	}
	return c.w.Flush()
}

func (c *yateClient) sendConnectGlobal() error {
	return c.sendLine("%%>connect:global")
}

func (c *yateClient) installHandler(name string, prio int) error {
	return c.sendLine(fmt.Sprintf("%%>install:%d:%s", prio, msgEscape(name, 0)))
}

func (c *yateClient) call(ctx context.Context, name string, params map[string]string) (yateMsg, error) {
	id := newMsgID()
	now := time.Now().Unix()
	var b strings.Builder
	b.WriteString("%%>message:")
	b.WriteString(msgEscape(id, 0))
	b.WriteString(":")
	b.WriteString(fmt.Sprintf("%d", now))
	b.WriteString(":")
	b.WriteString(msgEscape(name, 0))
	b.WriteString(":")
	b.WriteString("") // default retvalue empty
	for k, v := range params {
		// key is escaped with extraEsc='=' (like Yate param names)
		b.WriteString(":")
		b.WriteString(msgEscape(k, '='))
		b.WriteString("=")
		b.WriteString(msgEscape(v, 0))
	}

	ch := make(chan yateMsg, 1)
	c.mu.Lock()
	c.waiting[id] = ch
	c.mu.Unlock()

	if err := c.sendLine(b.String()); err != nil {
		c.mu.Lock()
		delete(c.waiting, id)
		c.mu.Unlock()
		return yateMsg{}, err
	}

	select {
	case <-ctx.Done():
		c.mu.Lock()
		delete(c.waiting, id)
		c.mu.Unlock()
		return yateMsg{}, ctx.Err()
	case m, ok := <-ch:
		if !ok {
			return yateMsg{}, errors.New("yate connection closed")
		}
		return m, nil
	}
}

func parseKVParams(rest string) (map[string]string, error) {
	out := map[string]string{}
	if rest == "" {
		return out, nil
	}
	parts := strings.Split(rest, ":")
	for _, p := range parts {
		if p == "" {
			continue
		}
		kv := strings.SplitN(p, "=", 2)
		kEsc := kv[0]
		k, err := msgUnescape(kEsc)
		if err != nil {
			return nil, err
		}
		if len(kv) == 1 {
			out[k] = ""
			continue
		}
		v, err := msgUnescape(kv[1])
		if err != nil {
			return nil, err
		}
		out[k] = v
	}
	return out, nil
}

func (c *yateClient) replyToEngine(id string, processed bool, name string, ret string, params map[string]string) error {
	var b strings.Builder
	b.WriteString("%%<message:")
	b.WriteString(msgEscape(id, 0))
	b.WriteString(":")
	if processed {
		b.WriteString("true")
	} else {
		b.WriteString("false")
	}
	b.WriteString(":")
	b.WriteString(msgEscape(name, 0))
	b.WriteString(":")
	b.WriteString(msgEscape(ret, 0))
	for k, v := range params {
		b.WriteString(":")
		b.WriteString(msgEscape(k, '='))
		if v == "" {
			continue
		}
		b.WriteString("=")
		b.WriteString(msgEscape(v, 0))
	}
	return c.sendLine(b.String())
}

func (c *yateClient) readLoop(onEngineMessage func(m yateMsg) (processed bool, ret string, params map[string]string)) {
	defer c.close()
	for {
		line, err := c.r.ReadString('\n')
		if err != nil {
			return
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "" {
			continue
		}
		if strings.HasPrefix(line, "%%<message:") {
			// response to our call()
			rest := strings.TrimPrefix(line, "%%<message:")
			// id:processed:[name]:ret[:k=v...]
			parts := strings.SplitN(rest, ":", 4)
			if len(parts) < 3 {
				continue
			}
			id, err := msgUnescape(parts[0])
			if err != nil {
				continue
			}
			procStr := parts[1]
			processed := procStr == "true"
			nameEsc := parts[2]
			name, _ := msgUnescape(nameEsc)

			retAndParams := ""
			if len(parts) == 4 {
				retAndParams = parts[3]
			}
			retPart := retAndParams
			paramPart := ""
			if idx := strings.Index(retAndParams, ":"); idx >= 0 {
				retPart = retAndParams[:idx]
				paramPart = retAndParams[idx+1:]
			}
			retVal, _ := msgUnescape(retPart)
			params, _ := parseKVParams(paramPart)

			c.mu.Lock()
			ch := c.waiting[id]
			delete(c.waiting, id)
			c.mu.Unlock()
			if ch != nil {
				ch <- yateMsg{ID: id, Processed: processed, Name: name, Ret: retVal, Params: params}
				close(ch)
			}
			continue
		}

		if strings.HasPrefix(line, "%%>message:") {
			// engine calling us (downlink)
			rest := strings.TrimPrefix(line, "%%>message:")
			// id:time:name:ret[:k=v...]
			parts := strings.SplitN(rest, ":", 4)
			if len(parts) < 3 {
				continue
			}
			id, err := msgUnescape(parts[0])
			if err != nil {
				continue
			}
			name, _ := msgUnescape(parts[2])
			// remaining
			retAndParams := ""
			if len(parts) == 4 {
				retAndParams = parts[3]
			}
			retPart := retAndParams
			paramPart := ""
			if idx := strings.Index(retAndParams, ":"); idx >= 0 {
				retPart = retAndParams[:idx]
				paramPart = retAndParams[idx+1:]
			}
			retVal, _ := msgUnescape(retPart)
			params, _ := parseKVParams(paramPart)
			proc, retOut, pOut := onEngineMessage(yateMsg{ID: id, Processed: true, Name: name, Ret: retVal, Params: params})
			_ = c.replyToEngine(id, proc, name, retOut, pOut)
			continue
		}

		// ignore other lines (install/watch/etc acks)
	}
}

type gateway struct {
	yate *yateClient

	mqttServer *mqtt.Server
}

type eventRow struct {
	ID       string `json:"id"`
	DeviceID string `json:"device_id"`
	Ts       int64  `json:"ts"`
	Kind     string `json:"kind"`
	Proto    string `json:"proto"`
	Payload  string `json:"payload"`
}

type commandRow struct {
	CommandID string `json:"command_id"`
	DeviceID  string `json:"device_id"`
	Payload   string `json:"payload"`
	Status    string `json:"status"`
	CreatedTs int64  `json:"created_ts"`
}

type alarmRow struct {
	ID             string `json:"id"`
	DeviceID       string `json:"device_id"`
	RuleID         string `json:"rule_id"`
	StartTs        int64  `json:"start_ts"`
	EndTs         *int64 `json:"end_ts,omitempty"`
	Level          string `json:"level"`
	Acked          int    `json:"acked"`
	AckTs         *int64 `json:"ack_ts,omitempty"`
	PayloadSnapshot string `json:"payload_snapshot"`
	CreatedTs      int64  `json:"created_ts"`
}

func parseEventLines(ret string) []eventRow {
	lines := strings.Split(strings.TrimSpace(ret), "\n")
	var out []eventRow
	for i, ln := range lines {
		ln = strings.TrimRight(ln, "\r")
		if i == 0 || ln == "" {
			continue
		}
		fields := strings.SplitN(ln, "\t", 6)
		for len(fields) < 6 {
			fields = append(fields, "")
		}
		var ts int64
		fmt.Sscanf(fields[2], "%d", &ts)
		out = append(out, eventRow{
			ID:       fields[0],
			DeviceID: fields[1],
			Ts:       ts,
			Kind:     fields[3],
			Proto:    fields[4],
			Payload:  fields[5],
		})
	}
	return out
}

func parseCommandLines(ret string) []commandRow {
	lines := strings.Split(strings.TrimSpace(ret), "\n")
	var out []commandRow
	for i, ln := range lines {
		ln = strings.TrimRight(ln, "\r")
		if i == 0 || ln == "" {
			continue
		}
		fields := strings.SplitN(ln, "\t", 5)
		for len(fields) < 5 {
			fields = append(fields, "")
		}
		var createdTs int64
		fmt.Sscanf(fields[4], "%d", &createdTs)
		out = append(out, commandRow{
			CommandID: fields[0],
			DeviceID:  fields[1],
			Payload:   fields[2],
			Status:    fields[3],
			CreatedTs: createdTs,
		})
	}
	return out
}

func parseAlarmLines(ret string) []alarmRow {
	lines := strings.Split(strings.TrimSpace(ret), "\n")
	var out []alarmRow
	for i, ln := range lines {
		ln = strings.TrimRight(ln, "\r")
		if i == 0 || ln == "" {
			continue
		}
		fields := strings.SplitN(ln, "\t", 10)
		for len(fields) < 10 {
			fields = append(fields, "")
		}
		var startTs, createdTs int64
		fmt.Sscanf(fields[3], "%d", &startTs)
		fmt.Sscanf(fields[9], "%d", &createdTs)
		var endTs, ackTs *int64
		if fields[4] != "" {
			var v int64
			if _, err := fmt.Sscanf(fields[4], "%d", &v); err == nil {
				endTs = &v
			}
		}
		if fields[7] != "" {
			var v int64
			if _, err := fmt.Sscanf(fields[7], "%d", &v); err == nil {
				ackTs = &v
			}
		}
		acked := 0
		fmt.Sscanf(fields[6], "%d", &acked)
		out = append(out, alarmRow{
			ID:              fields[0],
			DeviceID:        fields[1],
			RuleID:          fields[2],
			StartTs:         startTs,
			EndTs:           endTs,
			Level:           fields[5],
			Acked:           acked,
			AckTs:           ackTs,
			PayloadSnapshot: fields[8],
			CreatedTs:       createdTs,
		})
	}
	return out
}

func (g *gateway) yateAuth(ctx context.Context, device, token, proto, peer string) (bool, string) {
	resp, err := g.yate.call(ctx, "iot.auth", map[string]string{
		"device": device,
		"token":  token,
		"proto":  proto,
		"peer":   peer,
	})
	if err != nil {
		return false, err.Error()
	}
	if resp.Params["error"] != "" {
		return false, resp.Params["error"]
	}
	return resp.Ret == "true" || strings.EqualFold(resp.Ret, "true") || resp.Params["authed"] == "true", resp.Ret
}

func (g *gateway) yateUplink(ctx context.Context, device, token, proto, kind string, tsSec int64, payload []byte, authed bool) (bool, string) {
	p := map[string]string{
		"device":  device,
		"proto":   proto,
		"kind":    kind,
		"ts":      fmt.Sprintf("%d", tsSec),
		"payload": string(payload),
	}
	if token != "" {
		p["token"] = token
	}
	if authed {
		p["authed"] = "true"
	}
	resp, err := g.yate.call(ctx, "iot.uplink", p)
	if err != nil {
		return false, err.Error()
	}
	if resp.Params["error"] != "" {
		return false, resp.Params["error"]
	}
	if resp.Ret != "ok" {
		return false, resp.Ret
	}
	g.evaluateRulesAndWebhooks(ctx, device, kind, tsSec, payload)
	return true, resp.Ret
}

type ruleRow struct {
	RuleID     string `json:"rule_id"`
	Name       string `json:"name"`
	DeviceID   string `json:"device_id"`
	Kind       string `json:"kind"`
	KeyName    string `json:"key_name"`
	Op         string `json:"op"`
	Value      string `json:"value"`
	WebhookURL string `json:"webhook_url"`
	AlarmLevel string `json:"alarm_level"`
}

func parseRuleLines(ret string) []ruleRow {
	lines := strings.Split(strings.TrimSpace(ret), "\n")
	var out []ruleRow
	for i, ln := range lines {
		ln = strings.TrimRight(ln, "\r")
		if i == 0 || ln == "" {
			continue
		}
		fields := strings.SplitN(ln, "\t", 9)
		for len(fields) < 9 {
			fields = append(fields, "")
		}
		out = append(out, ruleRow{
			RuleID:     fields[0],
			Name:       fields[1],
			DeviceID:   fields[2],
			Kind:       fields[3],
			KeyName:    fields[4],
			Op:         fields[5],
			Value:      fields[6],
			WebhookURL: fields[7],
			AlarmLevel: fields[8],
		})
	}
	return out
}

func ruleMatches(payload map[string]any, keyName, op, value string) bool {
	raw, ok := payload[keyName]
	if !ok {
		return false
	}
	// Try numeric comparison first
	var numVal float64
	var numRule float64
	numValOK := false
	switch v := raw.(type) {
	case float64:
		numVal = v
		numValOK = true
	case int:
		numVal = float64(v)
		numValOK = true
	case int64:
		numVal = float64(v)
		numValOK = true
	}
	if _, err := fmt.Sscanf(value, "%f", &numRule); err == nil && numValOK {
		switch op {
		case "gt":
			return numVal > numRule
		case "gte":
			return numVal >= numRule
		case "lt":
			return numVal < numRule
		case "lte":
			return numVal <= numRule
		case "eq":
			return numVal == numRule
		case "ne":
			return numVal != numRule
		}
	}
	// String comparison
	strVal := fmt.Sprint(raw)
	switch op {
	case "eq":
		return strVal == value
	case "ne":
		return strVal != value
	case "gt", "gte", "lt", "lte":
		return false
	default:
		return false
	}
}

func (g *gateway) evaluateRulesAndWebhooks(ctx context.Context, device, kind string, tsSec int64, payload []byte) {
	resp, err := g.yate.call(ctx, "iot.rule.list", map[string]string{
		"device_id": device,
		"kind":      kind,
	})
	if err != nil || resp.Params["error"] != "" || resp.Ret == "error" {
		return
	}
	rules := parseRuleLines(resp.Ret)
	if len(rules) == 0 {
		return
	}
	var payloadMap map[string]any
	_ = json.Unmarshal(payload, &payloadMap)
	if payloadMap == nil {
		payloadMap = make(map[string]any)
	}
	payloadStr := string(payload)
	if len(payloadStr) > 8192 {
		payloadStr = payloadStr[:8192]
	}
	for _, r := range rules {
		if !ruleMatches(payloadMap, r.KeyName, r.Op, r.Value) {
			continue
		}
		level := r.AlarmLevel
		if level == "" {
			level = "warning"
		}
		_, _ = g.yate.call(ctx, "iot.alarm.create", map[string]string{
			"device_id":       device,
			"rule_id":         r.RuleID,
			"level":           level,
			"payload_snapshot": payloadStr,
		})
		if r.WebhookURL != "" {
			body := map[string]any{
				"device":     device,
				"rule_id":    r.RuleID,
				"kind":       kind,
				"ts":         tsSec,
				"level":      level,
				"payload":    payloadMap,
				"alarm_level": level,
			}
			b, _ := json.Marshal(body)
			req, err := http.NewRequestWithContext(ctx, http.MethodPost, r.WebhookURL, bytes.NewReader(b))
			if err != nil {
				continue
			}
			req.Header.Set("Content-Type", "application/json")
			client := &http.Client{Timeout: 5 * time.Second}
			_, _ = client.Do(req)
		}
	}
}

// ---- MQTT hook ----
type yateAuthHook struct {
	mqtt.HookBase
	gw *gateway
}

func (h *yateAuthHook) ID() string { return "yate-auth" }

func (h *yateAuthHook) Provides(b byte) bool {
	return b == mqtt.OnConnectAuthenticate || b == mqtt.OnPublish
}

func (h *yateAuthHook) OnConnectAuthenticate(cl *mqtt.Client, pk packets.Packet) bool {
	u := string(pk.Connect.Username)
	p := string(pk.Connect.Password)
	if u == "" || p == "" {
		return false
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	ok, _ := h.gw.yateAuth(ctx, u, p, "mqtt", cl.Net.Remote)
	return ok
}

func (h *yateAuthHook) OnPublish(cl *mqtt.Client, pk packets.Packet) (packets.Packet, error) {
	// Accept only telemetry-like topics. Device is the authenticated username.
	device := string(cl.Properties.Username)
	if device == "" {
		return pk, nil
	}
	topic := pk.TopicName
	kind := "telemetry"
	if strings.Contains(topic, "/attributes") {
		kind = "attributes"
	} else if strings.Contains(topic, "/heartbeat") {
		kind = "heartbeat"
	}
	payload := pk.Payload
	if len(payload) == 0 {
		payload = []byte("{}")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	_, _ = h.gw.yateUplink(ctx, device, "", "mqtt", kind, time.Now().Unix(), payload, true)
	return pk, nil
}

// ---- HTTP ----
func (g *gateway) httpHandler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte("ok"))
	})

	// Device management
	mux.HandleFunc("/api/v1/devices", func(w http.ResponseWriter, r *http.Request) {
		switch r.Method {
		case http.MethodGet:
			params := map[string]string{}
			if v := r.URL.Query().Get("limit"); v != "" {
				params["limit"] = v
			}
			if v := r.URL.Query().Get("offset"); v != "" {
				params["offset"] = v
			}
			ctx, cancel := context.WithTimeout(r.Context(), 10*time.Second)
			defer cancel()
			resp, err := g.yate.call(ctx, "iot.device.list", params)
			if err != nil || resp.Params["error"] != "" {
				http.Error(w, "yate error", http.StatusBadGateway)
				return
			}
			lines := strings.Split(strings.TrimSpace(resp.Ret), "\n")
			type devRow struct {
				DeviceID string `json:"device_id"`
				Name     string `json:"name"`
				Enabled  int    `json:"enabled"`
				LastSeen int64  `json:"last_seen"`
			}
			var out []devRow
			for i, ln := range lines {
				if i == 0 {
					continue
				}
				ln = strings.TrimRight(ln, "\r")
				if ln == "" {
					continue
				}
				fields := strings.Split(ln, "\t")
				for len(fields) < 4 {
					fields = append(fields, "")
				}
				enabled := 1
				fmt.Sscanf(fields[2], "%d", &enabled)
				var last int64
				fmt.Sscanf(fields[3], "%d", &last)
				out = append(out, devRow{DeviceID: fields[0], Name: fields[1], Enabled: enabled, LastSeen: last})
			}
			totalStr := resp.Params["total"]
			total := 0
			if totalStr != "" {
				fmt.Sscanf(totalStr, "%d", &total)
			}
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(map[string]any{"total": total, "data": out})
		case http.MethodPost:
			var req struct {
				Device string `json:"device"`
				Token  string `json:"token"`
				Name   string `json:"name"`
			}
			if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
				http.Error(w, "invalid json", http.StatusBadRequest)
				return
			}
			ctx, cancel := context.WithTimeout(r.Context(), 3*time.Second)
			defer cancel()
			resp, err := g.yate.call(ctx, "iot.device.create", map[string]string{
				"device": req.Device,
				"token":  req.Token,
				"name":   req.Name,
			})
			if err != nil || resp.Params["error"] != "" {
				http.Error(w, "yate error", http.StatusBadGateway)
				return
			}
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(map[string]any{"result": resp.Ret})
		default:
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		}
	})

	mux.HandleFunc("/api/v1/devices/", func(w http.ResponseWriter, r *http.Request) {
		pathAfter := strings.TrimPrefix(r.URL.Path, "/api/v1/devices/")
		pathAfter = strings.Trim(pathAfter, "/")
		parts := strings.SplitN(pathAfter, "/", 2)
		deviceID := parts[0]
		subPath := ""
		if len(parts) > 1 {
			subPath = parts[1]
		}
		if deviceID == "" {
			http.NotFound(w, r)
			return
		}

		// GET/POST /api/v1/devices/{id}/telemetry — query events
		if subPath == "telemetry" && r.Method == http.MethodGet {
			params := map[string]string{"device": deviceID}
			if v := r.URL.Query().Get("from"); v != "" {
				var n int64
				if _, err := fmt.Sscanf(v, "%d", &n); err == nil && n > 1e12 {
					n /= 1000 // milliseconds -> seconds
				}
				params["from_ts"] = fmt.Sprintf("%d", n)
			}
			if v := r.URL.Query().Get("to"); v != "" {
				var n int64
				if _, err := fmt.Sscanf(v, "%d", &n); err == nil && n > 1e12 {
					n /= 1000
				}
				params["to_ts"] = fmt.Sprintf("%d", n)
			}
			if v := r.URL.Query().Get("kind"); v != "" {
				params["kind"] = v
			}
			if v := r.URL.Query().Get("limit"); v != "" {
				params["limit"] = v
			}
			if v := r.URL.Query().Get("offset"); v != "" {
				params["offset"] = v
			}
			ctx, cancel := context.WithTimeout(r.Context(), 15*time.Second)
			defer cancel()
			resp, err := g.yate.call(ctx, "iot.event.query", params)
			if err != nil || resp.Params["error"] != "" {
				http.Error(w, "query failed", http.StatusBadGateway)
				return
			}
			format := r.URL.Query().Get("format")
			if format == "csv" {
				w.Header().Set("Content-Type", "text/csv; charset=utf-8")
				_, _ = w.Write([]byte(resp.Ret))
				return
			}
			events := parseEventLines(resp.Ret)
			totalStr := resp.Params["total"]
			total := 0
			if totalStr != "" {
				fmt.Sscanf(totalStr, "%d", &total)
			}
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(map[string]any{"total": total, "data": events})
			return
		}

		// GET /api/v1/devices/{id}/telemetry/latest — latest snapshot
		if subPath == "telemetry/latest" && r.Method == http.MethodGet {
			params := map[string]string{"device": deviceID}
			if v := r.URL.Query().Get("kind"); v != "" {
				params["kind"] = v
			}
			if v := r.URL.Query().Get("limit"); v != "" {
				params["limit"] = v
			}
			ctx, cancel := context.WithTimeout(r.Context(), 10*time.Second)
			defer cancel()
			resp, err := g.yate.call(ctx, "iot.event.latest", params)
			if err != nil || resp.Params["error"] != "" {
				http.Error(w, "query failed", http.StatusBadGateway)
				return
			}
			events := parseEventLines(resp.Ret)
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(map[string]any{"data": events})
			return
		}

		// GET /api/v1/devices/{id}/commands — device pull pending commands
		if subPath == "commands" && r.Method == http.MethodGet {
			token := r.Header.Get("X-Token")
			if token == "" {
				token = r.URL.Query().Get("token")
			}
			ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
			defer cancel()
			ok, errMsg := g.yateAuth(ctx, deviceID, token, "http", r.RemoteAddr)
			if !ok {
				http.Error(w, errMsg, http.StatusUnauthorized)
				return
			}
			params := map[string]string{"device": deviceID, "status": "pending"}
			if v := r.URL.Query().Get("limit"); v != "" {
				params["limit"] = v
			}
			resp, err := g.yate.call(ctx, "iot.command.list", params)
			if err != nil || resp.Params["error"] != "" {
				http.Error(w, "yate error", http.StatusBadGateway)
				return
			}
			commands := parseCommandLines(resp.Ret)
			for _, c := range commands {
				_, _ = g.yate.call(ctx, "iot.command.mark_sent", map[string]string{"command_id": c.CommandID})
			}
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(map[string]any{"data": commands})
			return
		}

		// POST /api/v1/devices/{id}/command — platform send one command (then downlink)
		if (subPath == "command" || subPath == "commands") && r.Method == http.MethodPost {
			body, _ := io.ReadAll(io.LimitReader(r.Body, 256*1024))
			if len(bytes.TrimSpace(body)) == 0 {
				body = []byte("{}")
			}
			payload := string(body)
			ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
			defer cancel()
			resp, err := g.yate.call(ctx, "iot.command.send", map[string]string{
				"device":  deviceID,
				"payload": payload,
			})
			if err != nil || resp.Params["error"] != "" {
				http.Error(w, "yate error", http.StatusBadGateway)
				return
			}
			commandID := resp.Ret
			if commandID == "" {
				commandID = resp.Params["command_id"]
			}
			if g.mqttServer != nil {
				topic := "iot/" + deviceID + "/cmd"
				_ = g.mqttServer.Publish(topic, body, false, 0)
				_, _ = g.yate.call(ctx, "iot.command.mark_sent", map[string]string{"command_id": commandID})
			}
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(map[string]any{"command_id": commandID, "result": "ok"})
			return
		}

		// GET /api/v1/devices/{id}/alarms — list alarms for device
		if subPath == "alarms" && r.Method == http.MethodGet {
			params := map[string]string{"device_id": deviceID}
			if v := r.URL.Query().Get("active_only"); v != "" && (v == "1" || strings.EqualFold(v, "true")) {
				params["active_only"] = "true"
			}
			if v := r.URL.Query().Get("limit"); v != "" {
				params["limit"] = v
			}
			if v := r.URL.Query().Get("offset"); v != "" {
				params["offset"] = v
			}
			ctx, cancel := context.WithTimeout(r.Context(), 10*time.Second)
			defer cancel()
			resp, err := g.yate.call(ctx, "iot.alarm.list", params)
			if err != nil || resp.Params["error"] != "" {
				http.Error(w, "yate error", http.StatusBadGateway)
				return
			}
			alarms := parseAlarmLines(resp.Ret)
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(map[string]any{"data": alarms})
			return
		}

		// GET/DELETE /api/v1/devices/{id} — device get/delete (no subpath)
		if subPath == "" {
			switch r.Method {
			case http.MethodGet:
				ctx, cancel := context.WithTimeout(r.Context(), 3*time.Second)
				defer cancel()
				resp, err := g.yate.call(ctx, "iot.device.get", map[string]string{"device": deviceID})
				if err != nil || resp.Params["error"] != "" {
					http.Error(w, "not found", http.StatusNotFound)
					return
				}
				w.Header().Set("Content-Type", "text/plain; charset=utf-8")
				_, _ = w.Write([]byte(resp.Ret))
				return
			case http.MethodDelete:
				ctx, cancel := context.WithTimeout(r.Context(), 3*time.Second)
				defer cancel()
				resp, err := g.yate.call(ctx, "iot.device.delete", map[string]string{"device": deviceID})
				if err != nil || resp.Params["error"] != "" {
					http.Error(w, "yate error", http.StatusBadGateway)
					return
				}
				w.Header().Set("Content-Type", "application/json")
				_ = json.NewEncoder(w).Encode(map[string]any{"result": resp.Ret})
				return
			}
		}

		http.NotFound(w, r)
	})

	// Rules: list (by device_id + kind), create, delete
	mux.HandleFunc("/api/v1/rules", func(w http.ResponseWriter, r *http.Request) {
		switch r.Method {
		case http.MethodGet:
			deviceID := r.URL.Query().Get("device_id")
			kind := r.URL.Query().Get("kind")
			if deviceID == "" || kind == "" {
				http.Error(w, "device_id and kind required", http.StatusBadRequest)
				return
			}
			ctx, cancel := context.WithTimeout(r.Context(), 10*time.Second)
			defer cancel()
			resp, err := g.yate.call(ctx, "iot.rule.list", map[string]string{"device_id": deviceID, "kind": kind})
			if err != nil || resp.Params["error"] != "" {
				http.Error(w, "yate error", http.StatusBadGateway)
				return
			}
			rules := parseRuleLines(resp.Ret)
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(map[string]any{"data": rules})
		case http.MethodPost:
			var req struct {
				RuleID     string `json:"rule_id"`
				Name       string `json:"name"`
				DeviceID   string `json:"device_id"`
				Kind       string `json:"kind"`
				KeyName    string `json:"key_name"`
				Op         string `json:"op"`
				Value      string `json:"value"`
				WebhookURL string `json:"webhook_url"`
				AlarmLevel string `json:"alarm_level"`
				Enabled    *bool  `json:"enabled"`
			}
			if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
				http.Error(w, "invalid json", http.StatusBadRequest)
				return
			}
			if req.RuleID == "" || req.DeviceID == "" || req.Kind == "" || req.KeyName == "" || req.Op == "" {
				http.Error(w, "rule_id, device_id, kind, key_name, op required", http.StatusBadRequest)
				return
			}
			params := map[string]string{
				"rule_id": req.RuleID, "device_id": req.DeviceID, "kind": req.Kind,
				"key_name": req.KeyName, "op": req.Op, "value": req.Value,
				"webhook_url": req.WebhookURL, "alarm_level": req.AlarmLevel,
			}
			if req.Name != "" {
				params["name"] = req.Name
			}
			if req.AlarmLevel != "" {
				params["alarm_level"] = req.AlarmLevel
			}
			if req.Enabled != nil && !*req.Enabled {
				params["enabled"] = "false"
			}
			ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
			defer cancel()
			resp, err := g.yate.call(ctx, "iot.rule.create", params)
			if err != nil || resp.Params["error"] != "" {
				http.Error(w, resp.Params["error"], http.StatusBadGateway)
				return
			}
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(map[string]any{"result": resp.Ret})
		default:
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		}
	})
	mux.HandleFunc("/api/v1/rules/", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodDelete {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		ruleID := strings.TrimPrefix(r.URL.Path, "/api/v1/rules/")
		ruleID = strings.Trim(ruleID, "/")
		if ruleID == "" {
			http.NotFound(w, r)
			return
		}
		ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
		defer cancel()
		resp, err := g.yate.call(ctx, "iot.rule.delete", map[string]string{"rule_id": ruleID})
		if err != nil || resp.Params["error"] != "" {
			http.Error(w, "yate error", http.StatusBadGateway)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{"result": resp.Ret})
	})

	// Alarm ack: POST /api/v1/alarms/{id}/ack
	mux.HandleFunc("/api/v1/alarms/", func(w http.ResponseWriter, r *http.Request) {
		pathAfter := strings.TrimPrefix(r.URL.Path, "/api/v1/alarms/")
		pathAfter = strings.Trim(pathAfter, "/")
		parts := strings.SplitN(pathAfter, "/", 2)
		alarmID := parts[0]
		suffix := ""
		if len(parts) > 1 {
			suffix = parts[1]
		}
		if alarmID == "" {
			http.NotFound(w, r)
			return
		}
		if suffix == "ack" && r.Method == http.MethodPost {
			ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
			defer cancel()
			resp, err := g.yate.call(ctx, "iot.alarm.ack", map[string]string{"alarm_id": alarmID})
			if err != nil || resp.Params["error"] != "" {
				http.Error(w, "yate error", http.StatusBadGateway)
				return
			}
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(map[string]any{"result": resp.Ret})
			return
		}
		http.NotFound(w, r)
	})

	// Telemetry ingestion
	mux.HandleFunc("/api/v1/", func(w http.ResponseWriter, r *http.Request) {
		// POST /api/v1/{device}/telemetry
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		path := strings.TrimPrefix(r.URL.Path, "/api/v1/")
		parts := strings.Split(strings.Trim(path, "/"), "/")
		if len(parts) != 2 {
			http.NotFound(w, r)
			return
		}
		device := parts[0]
		kind := parts[1]
		if kind != "telemetry" && kind != "attributes" && kind != "heartbeat" {
			http.NotFound(w, r)
			return
		}
		token := r.Header.Get("X-Token")
		if token == "" {
			token = r.URL.Query().Get("token")
		}
		body, _ := io.ReadAll(io.LimitReader(r.Body, maxHTTPBodySize))
		if len(bytes.TrimSpace(body)) == 0 {
			body = []byte("{}")
		}
		ctx, cancel := context.WithTimeout(r.Context(), 3*time.Second)
		defer cancel()
		ok, _ := g.yateUplink(ctx, device, token, "http", kind, time.Now().Unix(), body, false)
		if !ok {
			http.Error(w, "unauthorized or error", http.StatusUnauthorized)
			return
		}
		w.WriteHeader(http.StatusAccepted)
		_, _ = w.Write([]byte("ok"))
	})

	return mux
}

// ---- CoAP ----
func (g *gateway) startCoAP(addr string) error {
	router := coapmux.NewRouter()
	_ = router.Handle("/api/v1/{device}/{kind}", coapmux.HandlerFunc(func(w coapmux.ResponseWriter, r *coapmux.Message) {
		if r.Code() != codes.POST {
			_ = w.SetResponse(codes.NotFound, message.TextPlain, bytes.NewReader([]byte("not found")))
			return
		}
		if r.RouteParams == nil {
			_ = w.SetResponse(codes.NotFound, message.TextPlain, bytes.NewReader([]byte("not found")))
			return
		}
		device := r.RouteParams.Vars["device"]
		kind := r.RouteParams.Vars["kind"]
		if device == "" || (kind != "telemetry" && kind != "attributes" && kind != "heartbeat") {
			_ = w.SetResponse(codes.NotFound, message.TextPlain, bytes.NewReader([]byte("not found")))
			return
		}
		token := ""
		queries, _ := r.Options().Queries()
		for _, q := range queries {
			if strings.HasPrefix(q, "token=") {
				token = strings.TrimPrefix(q, "token=")
				break
			}
		}
		payload, _ := r.ReadBody()
		if payload == nil || len(bytes.TrimSpace(payload)) == 0 {
			payload = []byte("{}")
		}
		ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()
		ok, _ := g.yateUplink(ctx, device, token, "coap", kind, time.Now().Unix(), payload, false)
		if !ok {
			_ = w.SetResponse(codes.Unauthorized, message.TextPlain, bytes.NewReader([]byte("unauthorized")))
			return
		}
		_ = w.SetResponse(codes.Changed, message.TextPlain, bytes.NewReader([]byte("ok")))
	}))
	go func() {
		_ = coap.ListenAndServe("udp", addr, router)
	}()
	return nil
}

// maxHTTPBodySize is the maximum request body size for HTTP telemetry/attributes (bytes). Default 1MB.
var maxHTTPBodySize int64 = 1024 * 1024

func main() {
	var (
		yateAddr   = flag.String("yate", getenv("YATE_ADDR", "127.0.0.1:5040"), "Yate extmodule listener address (host:port)")
		httpAddr   = flag.String("http", getenv("HTTP_ADDR", ":8088"), "HTTP listen address")
		coapAddr   = flag.String("coap", getenv("COAP_ADDR", ":5683"), "CoAP listen address (UDP)")
		mqttAddr   = flag.String("mqtt", getenv("MQTT_ADDR", ":1883"), "MQTT listen address")
		maxBody    = flag.Int64("http-max-body", getenvInt64("HTTP_MAX_BODY", 1024*1024), "Max HTTP request body size for telemetry (bytes)")
	)
	flag.Parse()
	maxHTTPBodySize = *maxBody
	if maxHTTPBodySize < 256 {
		maxHTTPBodySize = 256
	}
	if maxHTTPBodySize > 32*1024*1024 {
		maxHTTPBodySize = 32 * 1024 * 1024
	}

	yc, err := dialYate(*yateAddr)
	if err != nil {
		log.Fatal(err)
	}
	if err := yc.sendConnectGlobal(); err != nil {
		log.Fatal(err)
	}

	gw := &gateway{yate: yc}
	go yc.readLoop(func(m yateMsg) (bool, string, map[string]string) {
		// Downlink from Yate (optional). Currently supports MQTT publish only via server.Publish.
		if m.Name != "iot.downlink" {
			return false, "", nil
		}
		if gw.mqttServer == nil {
			return true, "no_mqtt", map[string]string{"error": "mqtt not running"}
		}
		device := m.Params["device"]
		topic := m.Params["topic"]
		if topic == "" {
			topic = "iot/" + device + "/cmd"
		}
		payload := []byte(m.Params["payload"])
		if len(payload) == 0 {
			payload = []byte("{}")
		}
		_ = gw.mqttServer.Publish(topic, payload, false, 0)
		return true, "ok", nil
	})

	// Install downlink handler (engine -> gateway).
	_ = yc.installHandler("iot.downlink", 100)

	// MQTT server
	srv := mqtt.New(&mqtt.Options{InlineClient: true})
	if err := srv.AddHook(&yateAuthHook{gw: gw}, nil); err != nil {
		log.Fatal(err)
	}
	tcp := listeners.NewTCP(listeners.Config{ID: "tcp1", Address: *mqttAddr})
	if err := srv.AddListener(tcp); err != nil {
		log.Fatal(err)
	}
	gw.mqttServer = srv
	go func() {
		if err := srv.Serve(); err != nil {
			log.Printf("mqtt serve stopped: %v", err)
		}
	}()

	// HTTP server
	httpSrv := &http.Server{
		Addr:              *httpAddr,
		Handler:           gw.httpHandler(),
		ReadHeaderTimeout: 5 * time.Second,
	}
	go func() {
		log.Printf("http listening on %s", *httpAddr)
		if err := httpSrv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Printf("http server error: %v", err)
		}
	}()

	// CoAP server
	err = gw.startCoAP(*coapAddr)
	if err != nil {
		log.Fatal(err)
	}
	log.Printf("mqtt listening on %s", *mqttAddr)
	log.Printf("coap listening on %s", *coapAddr)

	// Wait
	waitSig()

	// Shutdown
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = httpSrv.Shutdown(ctx)
	srv.Close()
	yc.close()
}

func getenv(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func getenvInt64(k string, def int64) int64 {
	v := os.Getenv(k)
	if v == "" {
		return def
	}
	var n int64
	if _, err := fmt.Sscanf(v, "%d", &n); err != nil {
		return def
	}
	return n
}

func waitSig() {
	ch := make(chan os.Signal, 1)
	signal.Notify(ch, syscall.SIGINT, syscall.SIGTERM)
	<-ch
}

