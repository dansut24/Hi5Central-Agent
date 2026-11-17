// Hi5Central Go Streaming Lab: independent, read-only VP8/VP9 WebRTC comparison host.
// This DOES NOT enroll as an RMM agent, manage Windows services, or reuse C++ credentials.
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net/url"
	"os"
	"os/exec"
	"os/signal"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/dansut24/Hi5Central-Agent/Agent/go-streamlab/internal/ivf"
	"github.com/gorilla/websocket"
	"github.com/pion/webrtc/v4"
	"github.com/pion/webrtc/v4/pkg/media"
)

const defaultRelay = "wss://go-streamlab.hi5central.com/go-streamlab/ws?role=host"

type message struct {
	Type        string             `json:"type"`
	Code        string             `json:"code,omitempty"`
	SDP         string             `json:"sdp,omitempty"`
	Candidate   string             `json:"candidate,omitempty"`
	SDPMid      string             `json:"sdpMid,omitempty"`
	FPS         int                `json:"fps,omitempty"`
	MaxWidth    int                `json:"maxWidth,omitempty"`
	BitrateKbps int                `json:"bitrateKbps,omitempty"`
	Codec       string             `json:"codec,omitempty"`
	IceServers  []webrtc.ICEServer `json:"iceServers,omitempty"`
}

type host struct {
	ws      *websocket.Conn
	writeMu sync.Mutex
	mu      sync.Mutex
	peers   []*webrtc.PeerConnection
	current *session
	ice     []webrtc.ICEServer
	ffmpeg  string
}

type session struct {
	ctx               context.Context
	cancel            context.CancelFunc
	pc                *webrtc.PeerConnection
	track             *webrtc.TrackLocalStaticSample
	codec             string
	fps               int
	maxWidth          int
	bitrate           int
	start             time.Time
	frames            atomic.Int64
	bytes             atomic.Int64
	finished          atomic.Bool
	pendingCandidates []webrtc.ICECandidateInit
}

func (h *host) send(v interface{}) error {
	h.writeMu.Lock()
	defer h.writeMu.Unlock()
	return h.ws.WriteJSON(v)
}
func (h *host) event(event, detail string) {
	log.Printf("%s: %s", event, detail)
	_ = h.send(map[string]interface{}{"type": event, "message": detail})
}
func (h *host) stop() {
	h.mu.Lock()
	s := h.current
	h.current = nil
	h.mu.Unlock()
	if s != nil {
		s.finished.Store(true)
		s.cancel()
		_ = s.pc.Close()
	}
}
func (h *host) start(m message) error {
	h.stop()
	codec := strings.ToLower(strings.TrimSpace(m.Codec))
	if codec != "vp8" && codec != "vp9" {
		return errors.New("only VP8 and VP9 video modes are available in this build")
	}
	if m.FPS != 15 && m.FPS != 30 && m.FPS != 45 && m.FPS != 60 {
		return errors.New("FPS must be 15, 30, 45, or 60")
	}
	if m.MaxWidth != 0 && m.MaxWidth != 1280 && m.MaxWidth != 1600 && m.MaxWidth != 1920 {
		return errors.New("unsupported maximum width")
	}
	if m.BitrateKbps < 1000 || m.BitrateKbps > 20000 {
		return errors.New("bitrate out of range")
	}
	h.mu.Lock()
	ice := append([]webrtc.ICEServer(nil), h.ice...)
	h.mu.Unlock()
	if len(ice) == 0 {
		return errors.New("TURN credentials unavailable; no media connection permitted")
	}
	cfg := webrtc.Configuration{ICEServers: ice}
	pc, err := webrtc.NewPeerConnection(cfg)
	if err != nil {
		return fmt.Errorf("peer connection: %w", err)
	}
	var mime string
	if codec == "vp8" {
		mime = webrtc.MimeTypeVP8
	} else {
		mime = webrtc.MimeTypeVP9
	}
	track, err := webrtc.NewTrackLocalStaticSample(webrtc.RTPCodecCapability{MimeType: mime}, "video", "hi5-go-streamlab")
	if err != nil {
		_ = pc.Close()
		return err
	}
	sender, err := pc.AddTrack(track)
	if err != nil {
		_ = pc.Close()
		return err
	}
	ctx, cancel := context.WithCancel(context.Background())
	s := &session{ctx: ctx, cancel: cancel, pc: pc, track: track, codec: codec, fps: m.FPS, maxWidth: m.MaxWidth, bitrate: m.BitrateKbps, start: time.Now()}
	h.mu.Lock()
	h.current = s
	h.mu.Unlock()
	go func() {
		b := make([]byte, 1500)
		for {
			if _, _, err := sender.Read(b); err != nil {
				return
			}
		}
	}()
	pc.OnICECandidate(func(c *webrtc.ICECandidate) {
		if c == nil {
			return
		}
		v := c.ToJSON()
		mid := "video"
		if v.SDPMid != nil {
			mid = *v.SDPMid
		}
		_ = h.send(map[string]interface{}{"type": "ice_candidate", "candidate": v.Candidate, "sdpMid": mid})
	})
	pc.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		if s.finished.Load() {
			return
		}
		h.event("diagnostic", "WebRTC "+state.String())
		if state == webrtc.PeerConnectionStateFailed || state == webrtc.PeerConnectionStateClosed {
			s.cancel()
		}
	})
	offer, err := pc.CreateOffer(nil)
	if err != nil {
		h.stop()
		return err
	}
	if err = pc.SetLocalDescription(offer); err != nil {
		h.stop()
		return err
	}
	if err = h.send(map[string]interface{}{"type": "webrtc_offer", "sdp": offer.SDP}); err != nil {
		h.stop()
		return err
	}
	h.event("diagnostic", fmt.Sprintf("%s WebRTC offer sent; waiting for browser answer", strings.ToUpper(codec)))
	return nil
}

func (h *host) onAnswer(sdp string) error {
	h.mu.Lock()
	s := h.current
	h.mu.Unlock()
	if s == nil {
		return errors.New("no active stream")
	}
	if err := s.pc.SetRemoteDescription(webrtc.SessionDescription{Type: webrtc.SDPTypeAnswer, SDP: sdp}); err != nil {
		return err
	}
	for _, c := range s.pendingCandidates {
		if err := s.pc.AddICECandidate(c); err != nil {
			return err
		}
	}
	s.pendingCandidates = nil
	go h.produce(s)
	return nil
}

func (h *host) produce(s *session) {
	defer s.cancel()
	encoder := "libvpx"
	if s.codec == "vp9" {
		encoder = "libvpx-vp9"
	}
	args := []string{"-hide_banner", "-loglevel", "error", "-f", "gdigrab", "-draw_mouse", "1", "-framerate", fmt.Sprint(s.fps), "-i", "desktop"}
	if s.maxWidth > 0 {
		args = append(args, "-vf", fmt.Sprintf("scale=w=min(iw\\,%d):h=-2:flags=fast_bilinear", s.maxWidth))
	}
	args = append(args, "-an", "-pix_fmt", "yuv420p", "-c:v", encoder, "-deadline", "realtime", "-cpu-used", "8", "-threads", "4", "-b:v", fmt.Sprintf("%dk", s.bitrate), "-lag-in-frames", "0", "-g", fmt.Sprint(s.fps), "-error-resilient", "1")
	if s.codec == "vp9" {
		args = append(args, "-row-mt", "1")
	}
	args = append(args, "-f", "ivf", "pipe:1")
	cmd := exec.CommandContext(s.ctx, h.ffmpeg, args...)
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		h.event("error", "FFmpeg stdout: "+err.Error())
		return
	}
	// Limit stderr so malformed FFmpeg output cannot exhaust memory.
	stderr := &limitedWriter{max: 8192}
	cmd.Stderr = stderr
	if err = cmd.Start(); err != nil {
		h.event("error", "FFmpeg start: "+err.Error())
		return
	}
	done := make(chan error, 1)
	go func() { done <- cmd.Wait() }()
	reader, err := ivf.NewReader(stdout)
	if err != nil {
		if s.ctx.Err() == nil {
			h.event("error", "FFmpeg IVF video not produced: "+err.Error()+" "+stderr.String())
		}
		s.cancel()
		<-done
		return
	}
	if reader.Header.Codec != s.codec {
		h.event("error", "Encoder produced unexpected codec")
		s.cancel()
		<-done
		return
	}
	h.event("capture_started", fmt.Sprintf("Go video: %s %dx%d via FFmpeg gdigrab", strings.ToUpper(s.codec), reader.Header.Width, reader.Header.Height))
	_ = h.send(map[string]interface{}{"type": "capture_started", "codec": s.codec, "width": reader.Header.Width, "height": reader.Header.Height, "backend": "FFmpeg gdigrab → libvpx → Go/Pion WebRTC"})
	go h.report(s, reader.Header.Width, reader.Header.Height)
	duration := time.Second / time.Duration(s.fps)
	for s.ctx.Err() == nil {
		frame, _, e := reader.Next()
		if e != nil {
			if !errors.Is(e, io.EOF) && s.ctx.Err() == nil {
				h.event("error", "IVF frame read: "+e.Error())
			}
			break
		}
		if e = s.track.WriteSample(media.Sample{Data: frame, Duration: duration}); e != nil {
			if s.ctx.Err() == nil {
				h.event("error", "WebRTC sample write: "+e.Error())
			}
			break
		}
		s.frames.Add(1)
		s.bytes.Add(int64(len(frame)))
	}
	s.cancel()
	<-done
	if !s.finished.Swap(true) {
		h.event("diagnostic", "Video encoder stopped. "+stderr.String())
	}
}
func (h *host) report(s *session, w, hgt int) {
	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	var lastFrames, lastBytes int64
	for {
		select {
		case <-s.ctx.Done():
			return
		case <-tick.C:
			n, b := s.frames.Load(), s.bytes.Load()
			elapsed := time.Since(s.start).Seconds()
			if elapsed < 1 {
				elapsed = 1
			}
			_ = h.send(map[string]interface{}{"type": "stats", "stats": map[string]interface{}{
				"codec": s.codec, "configuredFps": s.fps, "lastSecondFps": n - lastFrames, "fpsMeasured": float64(n) / elapsed,
				"bitrateKbpsActual": 8 * (b - lastBytes) / 1000, "framesSent": n, "bytesEncoded": b,
				"width": w, "height": hgt, "captureBackend": "FFmpeg gdigrab → libvpx → Pion", "samplePipeline": "IVF compressed video → WebRTC RTP", "captureMsAvg": nil, "convertMsAvg": nil, "encodeMsAvg": nil,
			}})
			lastFrames, lastBytes = n, b
		}
	}
}
func (h *host) onCandidate(m message) error {
	h.mu.Lock()
	s := h.current
	h.mu.Unlock()
	if s == nil {
		return nil
	}
	mid := m.SDPMid
	c := webrtc.ICECandidateInit{Candidate: m.Candidate, SDPMid: &mid}
	if s.pc.RemoteDescription() == nil {
		s.pendingCandidates = append(s.pendingCandidates, c)
		return nil
	}
	return s.pc.AddICECandidate(c)
}
func (h *host) handle(m message) {
	var err error
	switch m.Type {
	case "hello_required":
		err = h.send(map[string]string{"type": "hello"})
	case "registered":
		fmt.Println("\nPAIRING CODE: " + m.Code + "\nOpen https://go-streamlab.hi5central.com and enter this code.\n")
	case "paired":
		h.mu.Lock()
		h.ice = m.IceServers
		h.mu.Unlock()
		h.event("diagnostic", "Viewer paired; choose VP8 or VP9 and Start")
	case "start":
		err = h.start(m)
	case "stop":
		h.stop()
		_ = h.send(map[string]string{"type": "stopped"})
	case "viewer_left":
		h.stop()
	case "webrtc_answer":
		err = h.onAnswer(m.SDP)
	case "ice_candidate":
		err = h.onCandidate(m)
	case "error":
		log.Printf("relay error: %v", m)
	}
	if err != nil {
		h.event("error", err.Error())
	}
}
func main() {
	if runtime.GOOS != "windows" {
		fmt.Fprintln(os.Stderr, "Go Streaming Lab host currently supports Windows desktop capture only")
		os.Exit(2)
	}
	wsURL := flag.String("relay", defaultRelay, "dedicated Go test WSS relay (must use TLS)")
	ffmpeg := flag.String("ffmpeg", "ffmpeg.exe", "FFmpeg binary containing gdigrab + libvpx + libvpx-vp9")
	flag.Parse()
	u, err := url.Parse(*wsURL)
	if err != nil || u.Scheme != "wss" || u.Host == "" {
		log.Fatal("relay must be a valid wss:// URL")
	}
	path, err := exec.LookPath(*ffmpeg)
	if err != nil {
		log.Fatal("FFmpeg not found. Install an approved FFmpeg Windows build or run -ffmpeg C:\\path\\to\\ffmpeg.exe; no changes made to your C++ agent")
	}
	// Verify encoders explicitly before pairing; fail instead of silently switching codec.
	check, err := exec.Command(path, "-hide_banner", "-encoders").CombinedOutput()
	if err != nil || !strings.Contains(string(check), "libvpx-vp9") || !strings.Contains(string(check), "libvpx ") {
		log.Fatal("FFmpeg binary must include libvpx (VP8) and libvpx-vp9 (VP9) video encoders")
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	log.Printf("Hi5Central isolated Go Streaming Lab (read-only) %s", runtime.GOARCH)
	log.Printf("connecting dedicated relay %s", u.Host)
	dial := websocket.Dialer{HandshakeTimeout: 15 * time.Second}
	ws, _, err := dial.DialContext(ctx, *wsURL, nil)
	if err != nil {
		log.Fatal(err)
	}
	h := &host{ws: ws, ffmpeg: path}
	defer func() { h.stop(); _ = ws.Close() }()
	_ = h.send(map[string]string{"type": "hello"})
	go func() { <-ctx.Done(); _ = ws.Close() }()
	ws.SetReadLimit(512 << 10)
	for {
		var m message
		if err = ws.ReadJSON(&m); err != nil {
			if ctx.Err() == nil {
				log.Printf("relay disconnected: %v", err)
			}
			return
		}
		h.handle(m)
	}
}

type limitedWriter struct {
	mu  sync.Mutex
	buf []byte
	max int
}

func (l *limitedWriter) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if len(l.buf) < l.max {
		n := l.max - len(l.buf)
		if n > len(p) {
			n = len(p)
		}
		l.buf = append(l.buf, p[:n]...)
	}
	return len(p), nil
}
func (l *limitedWriter) String() string { l.mu.Lock(); defer l.mu.Unlock(); return string(l.buf) }

var _ = json.Valid
