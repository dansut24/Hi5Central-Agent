package ivf

import (
    "bytes"
    "encoding/binary"
    "testing"
)

func TestReadVP8AndVP9(t *testing.T) {
    for _, tc := range []struct{ fourcc, codec string }{{"VP80", "vp8"}, {"VP90", "vp9"}} {
        var b [32]byte
        copy(b[:4], "DKIF")
        binary.LittleEndian.PutUint16(b[6:8], 32)
        copy(b[8:12], tc.fourcc)
        binary.LittleEndian.PutUint16(b[12:14], 1280)
        binary.LittleEndian.PutUint16(b[14:16], 720)
        binary.LittleEndian.PutUint32(b[16:20], 60)
        binary.LittleEndian.PutUint32(b[20:24], 1)
        f := make([]byte, 12+3)
        binary.LittleEndian.PutUint32(f[:4], 3)
        binary.LittleEndian.PutUint64(f[4:12], 123)
        copy(f[12:], []byte{1, 2, 3})
        r, err := NewReader(bytes.NewReader(append(b[:], f...)))
        if err != nil { t.Fatal(err) }
        if r.Header.Codec != tc.codec || r.Header.Width != 1280 { t.Fatalf("header: %+v", r.Header) }
        payload, ts, err := r.Next()
        if err != nil || ts != 123 || len(payload) != 3 { t.Fatalf("frame: %x %d %v", payload, ts, err) }
    }
}

func TestRejectOtherCodec(t *testing.T) {
    var b [32]byte
    copy(b[:4], "DKIF")
    binary.LittleEndian.PutUint16(b[6:8], 32)
    copy(b[8:12], "AV01")
    if _, err := NewReader(bytes.NewReader(b[:])); err == nil { t.Fatal("expected unsupported codec error") }
}
