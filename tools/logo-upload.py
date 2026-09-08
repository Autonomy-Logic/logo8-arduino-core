#!/usr/bin/env python3
"""
logo-upload.py — push a compiled application to the device's Ethernet loader.

Stop-and-wait UDP: WRQ -> AWRQ, then DATA/ADAT per block, then FIN -> AFIN.
Pure stdlib so it can be invoked directly from an Arduino platform.txt upload
recipe (arduino-cli). Exit code 0 on success.

Usage:
    logo-upload.py <firmware.bin> [--host 192.168.2.4] [--port 24]
                   [--timeout 1.0] [--retries 20] [--verbose]
"""
import argparse, socket, struct, sys, time, zlib

TAG_WRQ  = 0x31515257
TAG_AWRQ = 0x51525741
TAG_DATA = 0x31544144
TAG_ADAT = 0x54414441
TAG_FIN  = 0x314E4946
TAG_AFIN = 0x4E494641
TAG_IDENT  = 0x544E4449
TAG_AIDENT = 0x746E6469
TAG_REBOOT = 0x54424552
TAG_AREBT  = 0x74626572
ROLE_BOOTLOADER = 0
ROLE_APP = 1
ST_OK = 0
ST_NAMES = {0:"OK",1:"image too large",2:"erase failed",3:"out-of-order block",
            4:"flash write failed",5:"CRC mismatch",6:"protocol state error"}

def log(v, *a):
    if v: print(*a, file=sys.stderr)

def xchg(sock, host, port, payload, want_tag, timeout, retries, verbose, match=None):
    """Send payload, wait for a reply whose first u32 == want_tag (and optional
    match(fields)->bool). Retries on timeout. Returns the reply bytes."""
    for attempt in range(retries):
        try:
            sock.sendto(payload, (host, port))
        except OSError:
            # "No route to host" — device unreachable at L2 (e.g. it's running
            # a no-network app, or mid-reboot). Wait and retry; this lets the
            # uploader survive a power-cycle and catch the boot window.
            time.sleep(min(timeout, 0.5))
            log(verbose, f"  (unreachable, retry {attempt+1}/{retries})")
            continue
        sock.settimeout(timeout)
        try:
            while True:
                data, _ = sock.recvfrom(2048)
                if len(data) >= 4 and struct.unpack_from("<I", data, 0)[0] == want_tag:
                    if match is None or match(data):
                        return data
        except socket.timeout:
            log(verbose, f"  (timeout, retry {attempt+1}/{retries})")
        except OSError:
            time.sleep(min(timeout, 0.5))
    raise TimeoutError(f"no reply to tag 0x{want_tag:08X} after {retries} retries")

def ident(sock, host, port, timeout=0.6):
    """Return ROLE_BOOTLOADER / ROLE_APP, or None if nothing answers."""
    try:
        sock.sendto(struct.pack("<I", TAG_IDENT), (host, port))
        sock.settimeout(timeout)
        data, _ = sock.recvfrom(64)
        if len(data) >= 8 and struct.unpack_from("<I", data, 0)[0] == TAG_AIDENT:
            return struct.unpack_from("<I", data, 4)[0]
    except (socket.timeout, OSError):
        pass
    return None

def wait_for_bootloader(sock, host, port, tries=40, verbose=False):
    """Poll IDENT until the device answers from its loader (or give up)."""
    for _ in range(tries):
        if ident(sock, host, port, timeout=0.5) == ROLE_BOOTLOADER:
            print("[logo-upload] bootloader is up.")
            return True
    return False

def modbus_reboot(host, verbose, port=502, timeout=2.0):
    """Ask the running application to reboot into the loader
    TCP via the custom FC 0x4C (magic-guarded). The runtime is Ethernet-only and
    answers on a separate control channel, so this is how a running
    user program is put back into the loader for a re-flash — no power-cycle.

    ADU: MBAP[txn:2][proto:2=0][len:2][unit:1] + PDU[FC=0x4C][magic:4].
    Returns True if the command was sent (ack is optional: the device resets
    ~200 ms after replying, so a missing ack still means it accepted)."""
    magic = bytes((0xB0, 0x07, 0x10, 0xAD))
    pdu = bytes((0x4C,)) + magic
    mbap = struct.pack(">HHHB", 1, 0, len(pdu) + 1, 1)   # unit id 1
    try:
        with socket.create_connection((host, port), timeout=timeout) as c:
            c.sendall(mbap + pdu)
            c.settimeout(timeout)
            try:
                resp = c.recv(64)
            except socket.timeout:
                print("[logo-upload] Modbus reboot sent (no ack — device likely resetting).")
                return True
            if len(resp) >= 9 and resp[7] == 0x4C:
                if resp[8] == 0x7E:
                    print("[logo-upload] runtime accepted reboot (Modbus FC 0x4C).")
                    return True
                print(f"[logo-upload] runtime refused reboot (status 0x{resp[8]:02X}).")
                return False
            log(verbose, "  unexpected Modbus reply to FC 0x4C")
            return True
    except OSError as e:
        log(verbose, f"  Modbus connect failed: {e}")
        return False

def ensure_bootloader(sock, host, port, verbose):
    """Put the device into its loader by whatever channel it answers on:
    UDP management or Modbus TCP, then wait for the loader to come up."""
    role = ident(sock, host, port)
    if role == ROLE_BOOTLOADER:
        log(verbose, "  device is in the bootloader")
        return True
    if role == ROLE_APP:
        print("[logo-upload] app is running (UDP mgmt) — requesting reboot into bootloader...")
        try:
            sock.sendto(struct.pack("<I", TAG_REBOOT), (host, port))
        except OSError:
            pass
        # Wait for the device to come back in its loader (can take several seconds).
        if wait_for_bootloader(sock, host, port, verbose=verbose):
            return True
        print("[logo-upload] timed out waiting for the bootloader after reboot.")
        return False
    # No UDP reply: ask the running application over Modbus (FC 0x4C) to reboot
    # into its loader, then wait for the loader.
    print("[logo-upload] no UDP mgmt reply — trying Modbus reboot (FC 0x4C) at :502...")
    if modbus_reboot(host, verbose) and wait_for_bootloader(sock, host, port, verbose=verbose):
        return True
    log(verbose, "  proceeding — WRQ will still reach a bootloader in its boot window")
    return True   # nothing rebooted it; WRQ spam can still catch a power-cycle window

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("firmware")
    ap.add_argument("--host", default="192.168.2.4")
    ap.add_argument("--port", type=int, default=24)
    ap.add_argument("--timeout", type=float, default=1.0)
    ap.add_argument("--retries", type=int, default=20)
    ap.add_argument("--no-reboot", action="store_true",
                    help="skip the IDENT/REBOOT handshake (device already in bootloader)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    with open(args.firmware, "rb") as f:
        img = f.read()
    total = len(img)
    crc = zlib.crc32(img) & 0xFFFFFFFF
    print(f"[logo-upload] {args.firmware}: {total} bytes, crc32=0x{crc:08X} -> {args.host}:{args.port}")

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # If a user app is running, ask it to reboot into the bootloader first
        # (the network equivalent of the Arduino RTS reset).
        if not args.no_reboot:
            if not ensure_bootloader(s, args.host, args.port, args.verbose):
                return 5

        # WRQ. The device erases the target flash here, but a single page erase
        # is only a few ms, so a short timeout is fine — and keeping it short
        # lets WRQ spam ~2/sec to reliably catch the bootloader's ~2.5 s boot
        # window during a power-cycle transition.
        wrq = struct.pack("<III", TAG_WRQ, total, crc)
        rep = xchg(s, args.host, args.port, wrq, TAG_AWRQ,
                   min(args.timeout, 0.5), args.retries, args.verbose)
        _, status, blk = struct.unpack_from("<III", rep, 0)
        if status != ST_OK:
            print(f"[logo-upload] WRQ rejected: {ST_NAMES.get(status, status)}"); return 2
        log(args.verbose, f"  block size = {blk}")

        # DATA blocks, stop-and-wait
        nblocks = (total + blk - 1) // blk
        for seq in range(nblocks):
            chunk = img[seq*blk:(seq+1)*blk]
            pkt = struct.pack("<II", TAG_DATA, seq) + chunk
            rep = xchg(s, args.host, args.port, pkt, TAG_ADAT, args.timeout,
                       args.retries, args.verbose,
                       match=lambda d: struct.unpack_from("<I", d, 4)[0] == seq)
            _, _, status = struct.unpack_from("<III", rep, 0)
            if status != ST_OK:
                print(f"[logo-upload] block {seq} error: {ST_NAMES.get(status, status)}"); return 3
            if seq % 16 == 0 or seq == nblocks-1:
                pct = 100*(seq+1)//nblocks
                print(f"\r[logo-upload] {seq+1}/{nblocks} blocks ({pct}%)", end="", flush=True)
        print()

        # FIN -> device verifies CRC, records boot-info, jumps to the app.
        # The device may jump before its AFIN reaches us; treat a missing AFIN
        # as probable success rather than failure (the flash+jump already ran).
        try:
            rep = xchg(s, args.host, args.port, struct.pack("<I", TAG_FIN), TAG_AFIN,
                       max(args.timeout, 3.0), args.retries, args.verbose)
            _, status = struct.unpack_from("<II", rep, 0)
            if status != ST_OK:
                print(f"[logo-upload] FIN failed: {ST_NAMES.get(status, status)}"); return 4
            print("[logo-upload] success — device verified image and is starting the application.")
        except TimeoutError:
            print("[logo-upload] no final ACK — the device likely verified the image and "
                  "already jumped to the application. Check the device (relays/behavior).")
        return 0
    finally:
        s.close()

if __name__ == "__main__":
    sys.exit(main())
