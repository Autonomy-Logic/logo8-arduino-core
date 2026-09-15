package main

import (
	"bytes"
	"encoding/hex"
	"hash/crc32"
	"os"
	"testing"
)

// Golden vectors captured from the original Python uploader
// (struct.pack("<...") + zlib.crc32). The loader's firmware parses these byte
// for byte, so a change here is a protocol change, not a refactor.
func ramp(n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte(i % 256)
	}
	return b
}

func TestCRC32MatchesZlib(t *testing.T) {
	// zlib.crc32 and hash/crc32 IEEE are the same polynomial and the same
	// final xor; this pins that they agree on a real payload.
	if got, want := crc32.ChecksumIEEE(ramp(768)), uint32(0xB0C0DF2A); got != want {
		t.Fatalf("crc32(768B ramp) = 0x%08X, want 0x%08X", got, want)
	}
	if got := crc32.ChecksumIEEE(nil); got != 0 {
		t.Fatalf("crc32(empty) = 0x%08X, want 0", got)
	}
}

func TestEncodeWRQ(t *testing.T) {
	img := ramp(768)
	got := hex.EncodeToString(encodeWRQ(uint32(len(img)), crc32.ChecksumIEEE(img)))
	want := "57525131000300002adfc0b0"
	if got != want {
		t.Fatalf("WRQ = %s, want %s", got, want)
	}
}

func TestEncodeDATA(t *testing.T) {
	img := ramp(768)
	cases := []struct {
		seq  uint32
		from int
		want string
	}{
		{0, 0, "44415431000000000001020304050607"},
		{7, 56, "444154310700000038393a3b3c3d3e3f"},
	}
	for _, c := range cases {
		got := hex.EncodeToString(encodeDATA(c.seq, img[c.from:c.from+8]))
		if got != c.want {
			t.Errorf("DATA seq=%d = %s, want %s", c.seq, got, c.want)
		}
	}
}

func TestEncodeTags(t *testing.T) {
	for _, c := range []struct {
		name string
		tag  uint32
		want string
	}{
		{"FIN", tagFIN, "46494e31"},
		{"IDENT", tagIDENT, "49444e54"},
	} {
		if got := hex.EncodeToString(encodeTag(c.tag)); got != c.want {
			t.Errorf("%s = %s, want %s", c.name, got, c.want)
		}
	}
}

// The tags are ASCII when read little-endian; this catches a byte-order slip
// that a hand-copied constant would otherwise hide.
func TestTagsAreASCII(t *testing.T) {
	for _, c := range []struct {
		tag  uint32
		want string
	}{
		{tagWRQ, "WRQ1"}, {tagAWRQ, "AWRQ"},
		{tagDATA, "DAT1"}, {tagADAT, "ADAT"},
		{tagFIN, "FIN1"}, {tagAFIN, "AFIN"},
		{tagIDENT, "IDNT"}, {tagAIDENT, "idnt"},
		{tagREBOOT, "REBT"},
	} {
		if got := string(encodeTag(c.tag)); got != c.want {
			t.Errorf("tag 0x%08X = %q, want %q", c.tag, got, c.want)
		}
	}
}

func TestU32At(t *testing.T) {
	b := []byte{0x01, 0x02, 0x03, 0x04, 0x05}
	if v, ok := u32At(b, 0); !ok || v != 0x04030201 {
		t.Errorf("u32At(0) = 0x%08X ok=%v", v, ok)
	}
	if _, ok := u32At(b, 2); ok {
		t.Error("u32At past the end should report not-ok")
	}
	if _, ok := u32At(nil, 0); ok {
		t.Error("u32At(nil) should report not-ok")
	}
}

func TestStatusNames(t *testing.T) {
	want := map[uint32]string{
		0: "OK", 1: "image too large", 2: "erase failed",
		3: "out-of-order block", 4: "flash write failed",
		5: "CRC mismatch", 6: "protocol state error",
	}
	for k, v := range want {
		if got := statusName(k); got != v {
			t.Errorf("statusName(%d) = %q, want %q", k, got, v)
		}
	}
	if statusName(99) != "unknown status" {
		t.Error("unmapped status should not panic or map to OK")
	}
}

// Block partitioning must cover the image exactly, with a short final block.
func TestBlockPartitioning(t *testing.T) {
	for _, c := range []struct{ total, blk, wantBlocks uint32 }{
		{768, 256, 3}, {769, 256, 4}, {1, 256, 1}, {256, 256, 1},
	} {
		got := (c.total + c.blk - 1) / c.blk
		if got != c.wantBlocks {
			t.Errorf("total=%d blk=%d -> %d blocks, want %d", c.total, c.blk, got, c.wantBlocks)
		}
		var covered uint32
		for seq := uint32(0); seq < got; seq++ {
			end := (seq + 1) * c.blk
			if end > c.total {
				end = c.total
			}
			covered += end - seq*c.blk
		}
		if covered != c.total {
			t.Errorf("total=%d blk=%d covered %d bytes", c.total, c.blk, covered)
		}
	}
}

// arduino-cli invokes `logo-upload <file> --host <ip>`; flags after the path
// were silently ignored once. Pin both orders.
func TestSplitPositionalAcceptsEitherOrder(t *testing.T) {
	cases := []struct {
		name     string
		args     []string
		wantPos  string
		wantRest []string
	}{
		{
			"arduino-cli order (path first)",
			[]string{"fw.bin", "--host", "192.168.2.5", "--verbose"},
			"fw.bin",
			[]string{"--host", "192.168.2.5", "--verbose"},
		},
		{
			"flags first",
			[]string{"--host", "192.168.2.5", "--verbose", "fw.bin"},
			"fw.bin",
			[]string{"--host", "192.168.2.5", "--verbose"},
		},
		{
			"equals form keeps its value attached",
			[]string{"fw.bin", "--host=192.168.2.5", "--retries", "3"},
			"fw.bin",
			[]string{"--host=192.168.2.5", "--retries", "3"},
		},
		{
			"a flag value is never mistaken for the positional",
			[]string{"--host", "192.168.2.5", "fw.bin"},
			"fw.bin",
			[]string{"--host", "192.168.2.5"},
		},
		{
			"boolean flag before the path",
			[]string{"--no-reboot", "fw.bin"},
			"fw.bin",
			[]string{"--no-reboot"},
		},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			pos, rest := splitPositional(c.args)
			if pos != c.wantPos {
				t.Errorf("positional = %q, want %q", pos, c.wantPos)
			}
			if len(rest) != len(c.wantRest) {
				t.Fatalf("rest = %v, want %v", rest, c.wantRest)
			}
			for i := range rest {
				if rest[i] != c.wantRest[i] {
					t.Errorf("rest[%d] = %q, want %q", i, rest[i], c.wantRest[i])
				}
			}
		})
	}
}

// No default device address: a dropped --host must fail, not flash whatever
// answers at a fallback.
func TestHostHasNoDefault(t *testing.T) {
	src, err := os.ReadFile("main.go")
	if err != nil {
		t.Fatalf("cannot read main.go: %v", err)
	}
	if !bytes.Contains(src, []byte(`flag.String("host", "", `)) {
		t.Error(`--host must be declared with an empty default`)
	}
	if bytes.Contains(src, []byte("192.168.")) {
		t.Error("main.go must not carry a hardcoded device address")
	}
}
