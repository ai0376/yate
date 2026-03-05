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
		"device": device,
		"proto":  proto,
		"kind":   kind,
		"ts":     fmt.Sprintf("%d", tsSec),
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
	return resp.Ret == "ok", resp.Ret
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
		device := strings.TrimPrefix(r.URL.Path, "/api/v1/devices/")
		device = strings.Trim(device, "/")
		if device == "" {
			http.NotFound(w, r)
			return
		}
		switch r.Method {
		case http.MethodGet:
			ctx, cancel := context.WithTimeout(r.Context(), 3*time.Second)
			defer cancel()
			resp, err := g.yate.call(ctx, "iot.device.get", map[string]string{"device": device})
			if err != nil || resp.Params["error"] != "" {
				http.Error(w, "not found", http.StatusNotFound)
				return
			}
			w.Header().Set("Content-Type", "text/plain; charset=utf-8")
			_, _ = w.Write([]byte(resp.Ret))
		case http.MethodDelete:
			ctx, cancel := context.WithTimeout(r.Context(), 3*time.Second)
			defer cancel()
			resp, err := g.yate.call(ctx, "iot.device.delete", map[string]string{"device": device})
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

