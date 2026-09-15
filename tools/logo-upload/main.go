// logo-upload — push a compiled application to the device's Ethernet loader.
//
// Exit codes: 0 ok, 2 WRQ rejected, 3 block error, 4 FIN failed, 5 no
// bootloader, 6 programming lock on.
package main

import (
	"flag"
	"fmt"
	"hash/crc32"
	"io"
	"net"
	"os"
	"strings"
	"time"
)

var (
	stdout io.Writer = os.Stdout
	stderr io.Writer = os.Stderr
)

func outf(format string, a ...any) { fmt.Fprintf(stdout, format+"\n", a...) }

const (
	exitOK         = 0
	exitWRQ        = 2
	exitBlock      = 3
	exitFIN        = 4
	exitNoLoader   = 5
	exitLocked     = 6
	exitUsageError = 1
)

func main() { os.Exit(run()) }

func run() int {
	// No default: a dropped --host must fail, not flash whatever answers there.
	host := flag.String("host", "", "device IP address (required)")
	port := flag.Int("port", 24, "device management UDP port")
	timeout := flag.Float64("timeout", 1.0, "per-exchange timeout in seconds")
	retries := flag.Int("retries", 20, "retries per exchange")
	noReboot := flag.Bool("no-reboot", false, "skip the IDENT/REBOOT handshake (device already in bootloader)")
	verbose := flag.Bool("verbose", false, "log retries and protocol detail to stderr")
	flag.Usage = func() {
		fmt.Fprintf(stderr, "usage: logo-upload <firmware.bin> --host <IP> [--port N]\n"+
			"                   [--timeout S] [--retries N] [--no-reboot] [--verbose]\n\n"+
			"  --host is required: there is no default device address.\n")
	}
	firmware, rest := splitPositional(os.Args[1:])
	if err := flag.CommandLine.Parse(rest); err != nil {
		return exitUsageError
	}
	if firmware == "" || flag.NArg() != 0 {
		flag.Usage()
		return exitUsageError
	}
	if strings.TrimSpace(*host) == "" {
		fmt.Fprintln(stderr, "logo-upload: --host is required (no default device address)")
		flag.Usage()
		return exitUsageError
	}

	img, err := os.ReadFile(firmware)
	if err != nil {
		outf("[logo-upload] cannot read %s: %v", firmware, err)
		return exitUsageError
	}
	total := uint32(len(img))
	crc := crc32.ChecksumIEEE(img)
	outf("[logo-upload] %s: %d bytes, crc32=0x%08X -> %s:%d", firmware, total, crc, *host, *port)

	addr := &net.UDPAddr{IP: net.ParseIP(*host), Port: *port}
	if addr.IP == nil {
		// Not a literal address: resolve it.
		resolved, err := net.ResolveUDPAddr("udp4", net.JoinHostPort(*host, fmt.Sprint(*port)))
		if err != nil {
			outf("[logo-upload] cannot resolve %s: %v", *host, err)
			return exitUsageError
		}
		addr = resolved
	}
	sock, err := net.ListenUDP("udp4", nil)
	if err != nil {
		outf("[logo-upload] cannot open a UDP socket: %v", err)
		return exitUsageError
	}
	defer sock.Close()

	c := &conn{sock: sock, addr: addr, v: *verbose}
	base := time.Duration(*timeout * float64(time.Second))

	if !*noReboot {
		switch ensureBootloader(c, *host, *verbose) {
		case rebootLocked:
			return exitLocked
		case rebootFailed:
			return exitNoLoader
		}
	}

	// WRQ. The device erases the target flash here, but a page erase is only a
	// few ms, so a short timeout lets WRQ spam ~2/sec catch the loader's boot
	// window during a power-cycle.
	rep, err := c.exchange(encodeWRQ(total, crc), tagAWRQ, minDur(base, 500*time.Millisecond), *retries, nil)
	if err != nil {
		outf("[logo-upload] no reply to WRQ: %v", err)
		return exitWRQ
	}
	status, _ := u32At(rep, 4)
	blk, ok := u32At(rep, 8)
	if status != stOK {
		outf("[logo-upload] WRQ rejected: %s", statusName(status))
		return exitWRQ
	}
	if !ok || blk == 0 {
		outf("[logo-upload] WRQ returned an invalid block size")
		return exitWRQ
	}
	c.logf("block size = %d", blk)

	// DATA blocks, stop-and-wait.
	nblocks := (total + blk - 1) / blk
	for seq := uint32(0); seq < nblocks; seq++ {
		end := (seq + 1) * blk
		if end > total {
			end = total
		}
		want := seq
		rep, err := c.exchange(encodeDATA(seq, img[seq*blk:end]), tagADAT, base, *retries,
			func(d []byte) bool { got, ok := u32At(d, 4); return ok && got == want })
		if err != nil {
			outf("\n[logo-upload] block %d: %v", seq, err)
			return exitBlock
		}
		if st, _ := u32At(rep, 8); st != stOK {
			outf("\n[logo-upload] block %d error: %s", seq, statusName(st))
			return exitBlock
		}
		if seq%16 == 0 || seq == nblocks-1 {
			fmt.Fprintf(stdout, "\r[logo-upload] %d/%d blocks (%d%%)", seq+1, nblocks, 100*(seq+1)/nblocks)
		}
	}
	fmt.Fprintln(stdout)

	// FIN. The device verifies the image, records boot-info and jumps to it. It
	// may jump before its AFIN reaches us, so a missing AFIN is probable success.
	rep, err = c.exchange(encodeTag(tagFIN), tagAFIN, maxDur(base, 3*time.Second), *retries, nil)
	if err != nil {
		outf("[logo-upload] no final ACK — the device likely verified the image and already jumped to the application. Check the device (relays/behavior).")
		return exitOK
	}
	if st, _ := u32At(rep, 4); st != stOK {
		outf("[logo-upload] FIN failed: %s", statusName(st))
		return exitFIN
	}
	outf("[logo-upload] success — device verified image and is starting the application.")
	return exitOK
}

// splitPositional lifts the firmware path out so flags may follow it, which
// arduino-cli's recipe does and Go's flag package does not handle. A flag's own
// value (--host 1.2.3.4) is not mistaken for the positional.
func splitPositional(args []string) (string, []string) {
	takesValue := map[string]bool{"-host": true, "-port": true, "-timeout": true, "-retries": true}
	var positional string
	rest := make([]string, 0, len(args))
	for i := 0; i < len(args); i++ {
		a := args[i]
		if strings.HasPrefix(a, "-") {
			rest = append(rest, a)
			name := strings.TrimLeft(a, "-")
			if !strings.Contains(a, "=") && takesValue["-"+name] && i+1 < len(args) {
				i++
				rest = append(rest, args[i])
			}
			continue
		}
		if positional == "" {
			positional = a
			continue
		}
		rest = append(rest, a)
	}
	return positional, rest
}
