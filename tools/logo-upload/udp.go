package main

import (
	"errors"
	"fmt"
	"net"
	"time"
)

var errNoReply = errors.New("no reply")

// conn wraps the management socket and the peer address.
type conn struct {
	sock *net.UDPConn
	addr *net.UDPAddr
	v    bool
}

func (c *conn) logf(format string, a ...any) {
	if c.v {
		fmt.Fprintf(stderr, "  "+format+"\n", a...)
	}
}

// exchange sends payload and waits for a reply whose first u32 is wantTag and
// which satisfies match. It retries on timeout and on an unreachable peer, so
// an upload can survive a power-cycle and catch the loader's boot window.
func (c *conn) exchange(payload []byte, wantTag uint32, timeout time.Duration, retries int, match func([]byte) bool) ([]byte, error) {
	buf := make([]byte, 2048)
	for attempt := 0; attempt < retries; attempt++ {
		if _, err := c.sock.WriteToUDP(payload, c.addr); err != nil {
			// Unreachable at L2 (no-network app, or mid-reboot). Wait and retry.
			time.Sleep(minDur(timeout, 500*time.Millisecond))
			c.logf("(unreachable, retry %d/%d)", attempt+1, retries)
			continue
		}
		deadline := time.Now().Add(timeout)
		for {
			if err := c.sock.SetReadDeadline(deadline); err != nil {
				break
			}
			n, _, err := c.sock.ReadFromUDP(buf)
			if err != nil {
				if ne, ok := err.(net.Error); ok && ne.Timeout() {
					c.logf("(timeout, retry %d/%d)", attempt+1, retries)
				} else {
					time.Sleep(minDur(timeout, 500*time.Millisecond))
				}
				break
			}
			reply := buf[:n]
			if tag, ok := u32At(reply, 0); ok && tag == wantTag {
				if match == nil || match(reply) {
					out := make([]byte, n)
					copy(out, reply)
					return out, nil
				}
			}
			// Wrong tag: keep draining until the deadline rather than burning a retry.
		}
	}
	return nil, fmt.Errorf("%w to tag 0x%08X after %d retries", errNoReply, wantTag, retries)
}

// ident returns the device role, or false when nothing answers.
func (c *conn) ident(timeout time.Duration) (uint32, bool) {
	if _, err := c.sock.WriteToUDP(encodeTag(tagIDENT), c.addr); err != nil {
		return 0, false
	}
	buf := make([]byte, 64)
	if err := c.sock.SetReadDeadline(time.Now().Add(timeout)); err != nil {
		return 0, false
	}
	n, _, err := c.sock.ReadFromUDP(buf)
	if err != nil {
		return 0, false
	}
	if tag, ok := u32At(buf[:n], 0); ok && tag == tagAIDENT {
		if role, ok := u32At(buf[:n], 4); ok {
			return role, true
		}
	}
	return 0, false
}

// waitForBootloader polls IDENT until the loader answers.
func (c *conn) waitForBootloader(tries int) bool {
	for i := 0; i < tries; i++ {
		if role, ok := c.ident(500 * time.Millisecond); ok && role == roleBootloader {
			outf("[logo-upload] bootloader is up.")
			return true
		}
	}
	return false
}

func minDur(a, b time.Duration) time.Duration {
	if a < b {
		return a
	}
	return b
}

func maxDur(a, b time.Duration) time.Duration {
	if a > b {
		return a
	}
	return b
}
