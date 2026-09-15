package main

import (
	"encoding/binary"
	"fmt"
	"net"
	"time"
)

const (
	mbPort      = 502
	fcReboot    = 0x4C // reboot into the firmware bootloader (magic-guarded)
	fcLock      = 0x4D // read the programming-lock state (read-only)
	mbSuccess   = 0x7E
	mbLocked    = 0x6C // refused: the device's programming lock is engaged
	lockWait    = 5 * time.Second
	lockPoll    = 500 * time.Millisecond
	rebootMagic = "\xB0\x07\x10\xAD"
)

// rebootResult is the three-way outcome of asking the app to reboot.
type rebootResult int

const (
	rebootFailed rebootResult = iota
	rebootAccepted
	rebootLocked
)

// mbTxn runs one Modbus/TCP request. ok=false means unreachable; an empty PDU
// with ok=true means the device reset before answering.
func mbTxn(host string, pdu []byte, timeout time.Duration) ([]byte, bool) {
	addr := net.JoinHostPort(host, fmt.Sprint(mbPort))
	c, err := net.DialTimeout("tcp", addr, timeout)
	if err != nil {
		return nil, false
	}
	defer c.Close()

	adu := make([]byte, 7+len(pdu))
	binary.BigEndian.PutUint16(adu[0:], 1)                  // transaction
	binary.BigEndian.PutUint16(adu[2:], 0)                  // protocol
	binary.BigEndian.PutUint16(adu[4:], uint16(len(pdu)+1)) // length
	adu[6] = 1                                              // unit
	copy(adu[7:], pdu)

	if err := c.SetWriteDeadline(time.Now().Add(timeout)); err != nil {
		return nil, false
	}
	if _, err := c.Write(adu); err != nil {
		return nil, false
	}
	if err := c.SetReadDeadline(time.Now().Add(timeout)); err != nil {
		return nil, false
	}
	buf := make([]byte, 64)
	n, err := c.Read(buf)
	if err != nil {
		// Reset before answering still counts as reached.
		return []byte{}, true
	}
	if n < 8 {
		return nil, false
	}
	return buf[7:n], true
}

// lockState reports 1 locked, 0 unlocked. ok=false means no usable answer.
// Read-only by design: polling does not re-raise the prompt on the device.
func lockState(host string, timeout time.Duration) (byte, bool) {
	pdu, reached := mbTxn(host, []byte{fcLock}, timeout)
	if !reached || len(pdu) < 3 {
		return 0, false
	}
	if pdu[0] == fcLock && pdu[1] == mbSuccess {
		return pdu[2], true
	}
	return 0, false
}

// modbusReboot asks the running app to reboot into the loader via FC 0x4C. A
// locked device asks on its own display, so keep polling for lockWait.
func modbusReboot(host string, v bool, timeout time.Duration) rebootResult {
	pdu := append([]byte{fcReboot}, []byte(rebootMagic)...)

	ask := func() rebootResult {
		resp, reached := mbTxn(host, pdu, timeout)
		if !reached {
			if v {
				fmt.Fprintln(stderr, "  Modbus connect failed")
			}
			return rebootFailed
		}
		if len(resp) == 0 {
			// The device resets ~200 ms after replying, so a missing ack still
			// means it accepted.
			outf("[logo-upload] Modbus reboot sent (no ack — device likely resetting).")
			return rebootAccepted
		}
		if len(resp) >= 2 && resp[0] == fcReboot {
			switch resp[1] {
			case mbSuccess:
				outf("[logo-upload] runtime accepted reboot (Modbus FC 0x4C).")
				return rebootAccepted
			case mbLocked:
				return rebootLocked
			}
			outf("[logo-upload] runtime refused reboot (status 0x%02X).", resp[1])
			return rebootFailed
		}
		if v {
			fmt.Fprintln(stderr, "  unexpected Modbus reply to FC 0x4C")
		}
		return rebootAccepted
	}

	if r := ask(); r != rebootLocked {
		return r
	}

	outf("[logo-upload] device is LOCKED — the program lock is on.")
	outf("[logo-upload] It is now asking on its own display: press OK on the device to unlock (waiting up to %.0fs)...", lockWait.Seconds())

	deadline := time.Now().Add(lockWait)
	for {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			break
		}
		time.Sleep(minDur(lockPoll, remaining))
		state, ok := lockState(host, time.Second)
		if ok && state == 0 {
			outf("[logo-upload] unlocked at the device — retrying the reboot.")
			if r := ask(); r != rebootLocked {
				return r
			}
			// Raced with a re-lock; keep waiting out the window.
		} else if !ok {
			// Either it went away or it already rebooted on an earlier accepted
			// attempt; let the caller's IDENT poll decide.
			outf("[logo-upload] device stopped answering Modbus — checking the loader.")
			return rebootAccepted
		}
		left := time.Until(deadline).Seconds()
		if left < 0 {
			left = 0
		}
		outf("[logo-upload]   still locked, %.1fs left...", left)
	}

	outf("[logo-upload] device is still locked. Unlock it at the panel (Program lock -> Off, or answer the on-screen prompt) and upload again.")
	return rebootLocked
}

// ensureBootloader puts the device into its loader over UDP mgmt, or Modbus
// TCP for an Ethernet-only application.
func ensureBootloader(c *conn, host string, v bool) rebootResult {
	if role, ok := c.ident(600 * time.Millisecond); ok {
		switch role {
		case roleBootloader:
			c.logf("device is in the bootloader")
			return rebootAccepted
		case roleApp:
			outf("[logo-upload] app is running (UDP mgmt) — requesting reboot into bootloader...")
			_, _ = c.sock.WriteToUDP(encodeTag(tagREBOOT), c.addr)
			if c.waitForBootloader(40) {
				return rebootAccepted
			}
			outf("[logo-upload] timed out waiting for the bootloader after reboot.")
			return rebootFailed
		}
	}

	outf("[logo-upload] no UDP mgmt reply — trying Modbus reboot (FC 0x4C) at :502...")
	switch modbusReboot(host, v, 2*time.Second) {
	case rebootLocked:
		// A definite answer, not a maybe: spamming WRQ would only produce a
		// confusing timeout.
		return rebootLocked
	case rebootAccepted:
		if c.waitForBootloader(40) {
			return rebootAccepted
		}
	}
	c.logf("proceeding — WRQ will still reach a bootloader in its boot window")
	return rebootAccepted
}
