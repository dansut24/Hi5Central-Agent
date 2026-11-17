// Package ivf reads FFmpeg's IVF container without buffering whole video files.
// The payload of each IVF frame is a compressed VP8 or VP9 access unit, never JPEG.
package ivf

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

type Header struct {
	Codec                    string
	Width, Height            int
	TimebaseNum, TimebaseDen uint32
}

type Reader struct {
	src    io.Reader
	Header Header
}

func NewReader(src io.Reader) (*Reader, error) {
	var b [32]byte
	if _, err := io.ReadFull(src, b[:]); err != nil {
		return nil, err
	}
	if string(b[:4]) != "DKIF" || binary.LittleEndian.Uint16(b[4:6]) != 0 || binary.LittleEndian.Uint16(b[6:8]) != 32 {
		return nil, errors.New("invalid IVF header")
	}
	var codec string
	switch string(b[8:12]) {
	case "VP80":
		codec = "vp8"
	case "VP90":
		codec = "vp9"
	default:
		return nil, fmt.Errorf("unsupported IVF codec %q", string(b[8:12]))
	}
	w, h := int(binary.LittleEndian.Uint16(b[12:14])), int(binary.LittleEndian.Uint16(b[14:16]))
	if w < 16 || h < 16 || w > 16384 || h > 16384 {
		return nil, errors.New("invalid IVF dimensions")
	}
	den, num := binary.LittleEndian.Uint32(b[16:20]), binary.LittleEndian.Uint32(b[20:24])
	if den == 0 || num == 0 {
		return nil, errors.New("invalid IVF timebase")
	}
	return &Reader{src: src, Header: Header{Codec: codec, Width: w, Height: h, TimebaseNum: num, TimebaseDen: den}}, nil
}

// Next reads a single compressed frame. The timestamp is in IVF timebase ticks.
func (r *Reader) Next() ([]byte, uint64, error) {
	var h [12]byte
	if _, err := io.ReadFull(r.src, h[:]); err != nil {
		return nil, 0, err
	}
	n := binary.LittleEndian.Uint32(h[:4])
	if n == 0 || n > 8<<20 {
		return nil, 0, errors.New("invalid or oversized IVF frame")
	}
	b := make([]byte, int(n))
	if _, err := io.ReadFull(r.src, b); err != nil {
		return nil, 0, err
	}
	return b, binary.LittleEndian.Uint64(h[4:]), nil
}
