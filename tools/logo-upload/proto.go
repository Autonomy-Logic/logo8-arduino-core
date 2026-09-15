// Wire protocol for the LOGO! Ethernet loader.
//
// Stop-and-wait UDP: WRQ -> AWRQ, then DATA/ADAT per block, then FIN -> AFIN.
// Every field is little-endian on the wire.
package main

import "encoding/binary"

const (
	tagWRQ    uint32 = 0x31515257
	tagAWRQ   uint32 = 0x51525741
	tagDATA   uint32 = 0x31544144
	tagADAT   uint32 = 0x54414441
	tagFIN    uint32 = 0x314E4946
	tagAFIN   uint32 = 0x4E494641
	tagIDENT  uint32 = 0x544E4449
	tagAIDENT uint32 = 0x746E6469
	tagREBOOT uint32 = 0x54424552
)

const (
	roleBootloader uint32 = 0
	roleApp        uint32 = 1
)

const stOK uint32 = 0

// statusName mirrors ST_NAMES in the original uploader; the loader returns
// these in AWRQ, ADAT and AFIN.
func statusName(s uint32) string {
	switch s {
	case 0:
		return "OK"
	case 1:
		return "image too large"
	case 2:
		return "erase failed"
	case 3:
		return "out-of-order block"
	case 4:
		return "flash write failed"
	case 5:
		return "CRC mismatch"
	case 6:
		return "protocol state error"
	}
	return "unknown status"
}

func putU32(b []byte, off int, v uint32) { binary.LittleEndian.PutUint32(b[off:], v) }

// u32At reads the little-endian u32 at byte offset off, reporting whether the
// buffer was long enough.
func u32At(b []byte, off int) (uint32, bool) {
	if len(b) < off+4 {
		return 0, false
	}
	return binary.LittleEndian.Uint32(b[off:]), true
}

func encodeTag(tag uint32) []byte {
	b := make([]byte, 4)
	putU32(b, 0, tag)
	return b
}

// encodeWRQ: [tag][total][crc32]
func encodeWRQ(total, crc uint32) []byte {
	b := make([]byte, 12)
	putU32(b, 0, tagWRQ)
	putU32(b, 4, total)
	putU32(b, 8, crc)
	return b
}

// encodeDATA: [tag][seq][payload...]
func encodeDATA(seq uint32, chunk []byte) []byte {
	b := make([]byte, 8+len(chunk))
	putU32(b, 0, tagDATA)
	putU32(b, 4, seq)
	copy(b[8:], chunk)
	return b
}
