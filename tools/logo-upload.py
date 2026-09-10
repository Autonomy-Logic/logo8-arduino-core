#!/usr/bin/env python3
"""
logo-upload.py — push a compiled application to the device's Ethernet loader.

Stop-and-wait UDP: WRQ -> AWRQ, then DATA/ADAT per block, then FIN -> AFIN.
Pure stdlib so it can be invoked directly from an Arduino platform.txt upload
recipe (arduino-cli). Exit code 0 on success; 6 means the device refused
because its programming lock is on (see modbus_reboot).

Usage:
    logo-upload.py <firmware.bin> [--host 192.168.2.4] [--port 24]
                   [--timeout 1.0] [--retries 20] [--verbose]
"""
import argparse, socket, struct, sys, time, zlib

# arduino-cli runs us with stdout on a pipe, which makes Python block-buffer it:
# the editor would then get the whole log in one burst at the end, and progress
# printed during a wait would arrive only after the wait was over — exactly the
# "is it hung?" impression the progress exists to prevent. Line-buffer instead.
try:
    sys.stdout.reconfigure(line_buffering=True)
except AttributeError:        # Python < 3.7
    pass

TAG_BWRQ = 0x51525742
TAG_ABWRQ= 0x71727762
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
            4:"flash write failed",5:"CRC mismatch",6:"protocol state error",
            7:"wrong target for this device role"}

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

MB_PORT      = 502
FC_REBOOT    = 0x4C          # reboot into the firmware bootloader (magic-guarded)
FC_LOCK      = 0x4D          # read the programming-lock state (read-only)
MB_SUCCESS   = 0x7E
MB_LOCKED    = 0x6C          # refused: the device's programming lock is engaged
LOCK_WAIT_S  = 5.0           # how long to keep asking while the user unlocks
LOCK_POLL_S  = 0.5

def _mb_txn(host, pdu, port=MB_PORT, timeout=2.0):
    """One Modbus/TCP request-response. Returns the response PDU, b"" if the
    device reset before answering, or None if it could not be reached."""
    mbap = struct.pack(">HHHB", 1, 0, len(pdu) + 1, 1)   # txn 1, proto 0, unit 1
    try:
        with socket.create_connection((host, port), timeout=timeout) as c:
            c.sendall(mbap + pdu)
            c.settimeout(timeout)
            try:
                resp = c.recv(64)
            except socket.timeout:
                return b""
    except OSError:
        return None
    return resp[7:] if len(resp) >= 8 else None

def lock_state(host, port=MB_PORT, timeout=1.0):
    """FC 0x4D — 1 = locked, 0 = unlocked, None = no answer.

    Read-only by design: polling it does NOT re-raise the unlock prompt on the
    device's display, so we can watch for the user's answer without spamming
    their screen."""
    pdu = _mb_txn(host, bytes((FC_LOCK,)), port, timeout)
    if pdu and len(pdu) >= 3 and pdu[0] == FC_LOCK and pdu[1] == MB_SUCCESS:
        return pdu[2]
    return None

def modbus_reboot(host, verbose, port=MB_PORT, timeout=2.0, lock_wait=LOCK_WAIT_S):
    """Ask the running application to reboot into the loader via the custom
    Modbus FC 0x4C (magic-guarded). The application is Ethernet-only and
    answers on a separate control channel, so this is how a running
    user program is put back into the loader for a re-flash — no power-cycle.

    ADU: MBAP[txn:2][proto:2=0][len:2][unit:1] + PDU[FC=0x4C][magic:4].

    Three outcomes:
      True       accepted — the device is rebooting into the loader.
      "locked"   refused: the device's programming lock is engaged. The device
                 puts the question on its own display, so we keep
                 asking for `lock_wait` seconds to give whoever is standing
                 there time to press OK — printing progress the whole time, so
                 a wait that is really "waiting for a human" never looks like a
                 hang. If they don't, the caller reports a locked device.
      False      could not be reached, or refused for another reason.
    """
    magic = bytes((0xB0, 0x07, 0x10, 0xAD))
    pdu = bytes((FC_REBOOT,)) + magic

    def ask():
        """Send FC 0x4C once. Returns True/"locked"/False as above."""
        resp = _mb_txn(host, pdu, port, timeout)
        if resp is None:
            log(verbose, "  Modbus connect failed")
            return False
        if resp == b"":
            # The device resets ~200 ms after replying, so a missing ack still
            # means it accepted.
            print("[logo-upload] Modbus reboot sent (no ack — device likely resetting).")
            return True
        if len(resp) >= 2 and resp[0] == FC_REBOOT:
            if resp[1] == MB_SUCCESS:
                print("[logo-upload] runtime accepted reboot (Modbus FC 0x4C).")
                return True
            if resp[1] == MB_LOCKED:
                return "locked"
            print(f"[logo-upload] runtime refused reboot (status 0x{resp[1]:02X}).")
            return False
        log(verbose, "  unexpected Modbus reply to FC 0x4C")
        return True

    r = ask()
    if r != "locked":
        return r

    print(f"[logo-upload] device is LOCKED — the program lock is on.")
    print(f"[logo-upload] It is now asking on its own display: press OK on the "
          f"device to unlock (waiting up to {lock_wait:.0f}s)...")
    deadline = time.time() + lock_wait
    while True:
        remaining = deadline - time.time()
        if remaining <= 0:
            break
        time.sleep(min(LOCK_POLL_S, remaining))
        state = lock_state(host, port)
        if state == 0:
            print("[logo-upload] unlocked at the device — retrying the reboot.")
            r = ask()
            if r != "locked":
                return r
            # Raced with a re-lock; keep waiting out the window.
        elif state is None:
            # No answer. Either the device went away or it already rebooted on
            # an earlier accepted attempt; let the caller's IDENT poll decide.
            print("[logo-upload] device stopped answering Modbus — checking the loader.")
            return True
        print(f"[logo-upload]   still locked, {max(0.0, deadline - time.time()):.1f}s left...")

    print("[logo-upload] device is still locked. Unlock it at the panel "
          "(Program lock -> Off, or answer the on-screen prompt) and upload again.")
    return "locked"

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
    r = modbus_reboot(host, verbose)
    if r == "locked":
        # A locked device is a definite answer, not a maybe: spamming WRQ at it
        # would only produce a confusing timeout, so stop here and say why.
        return "locked"
    if r and wait_for_bootloader(sock, host, port, verbose=verbose):
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
    ap.add_argument("--target", choices=("app", "base"), default="app",
                    help="app  = the user application, flashed BY THE LOADER "
                         "(default); base = the resident loader itself, flashed "
                         "BY THE RUNNING APPLICATION. `base` takes the packaged "
                         "update container, not a bare .bin, because it also "
                         "carries the firmware manifest the device stores "
                         "alongside the image.")
    ap.add_argument("--no-reboot", action="store_true",
                    help="skip the IDENT/REBOOT handshake (device already in bootloader)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    with open(args.firmware, "rb") as f:
        img = f.read()
    total = len(img)
    crc = zlib.crc32(img) & 0xFFFFFFFF
    print(f"[logo-upload] {args.firmware}: {total} bytes, crc32=0x{crc:08X} -> {args.host}:{args.port}")

    base = args.target == "base"
    if base:
        # Sanity-check the container before touching the device: it must be the
        # packaged update (payload + 120-byte manifest), because a bare .bin
        # would leave the device's firmware manifest describing the OLD image.
        if len(img) < 136 or struct.unpack_from("<I", img, len(img) - 4)[0] != 0xAAAAAAAA:
            print("[logo-upload] --target base needs the PACKAGED update "
                  "container, not a bare firmware .bin.")
            return 7
        plen, pcrc, pload = struct.unpack_from("<III", img, len(img) - 16)
        print(f"[logo-upload] BASE container: payload {plen} bytes -> 0x{pload:05X}, "
              f"crc32=0x{pcrc:08X}, manifest -> 0xFC000")

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        if base:
            # The BASE is flashed BY THE RUNNING APPLICATION (a loader cannot
            # erase the pages it executes from), so here we need the app up —
            # the exact opposite of an app upload. Say so rather than silently
            # timing out against a device sitting in its loader.
            role = ident(s, args.host, args.port)
            if role == ROLE_BOOTLOADER:
                print("[logo-upload] the device is in its LOADER. A BASE update has "
                      "to be driven by the running application — upload an application "
                      "first, then retry.")
                return 5
            if role is None:
                print("[logo-upload] no reply on the management port — the running "
                      "application must serve it to accept a BASE update.")
                return 5
        elif not args.no_reboot:
            # If a user app is running, ask it to reboot into the bootloader
            # first (the network equivalent of the Arduino RTS reset).
            r = ensure_bootloader(s, args.host, args.port, args.verbose)
            if r == "locked":
                return 6
            if not r:
                return 5

        # WRQ/BWRQ. The device erases the target flash here, but a single page
        # erase is only a few ms, so a short timeout is fine — and keeping it
        # short lets WRQ spam ~2/sec to reliably catch the bootloader's ~2.5 s
        # boot window during a power-cycle transition.
        wrq = struct.pack("<III", TAG_BWRQ if base else TAG_WRQ, total, crc)
        rep = xchg(s, args.host, args.port, wrq, TAG_ABWRQ if base else TAG_AWRQ,
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

        # FIN -> the device verifies what it received and only then commits.
        # For an app upload it records boot-info and jumps; for a BASE update it
        # erases and reprograms the bootloader region from RAM and resets. Either
        # way it may act before its AFIN reaches us, so a missing AFIN is treated
        # as probable success rather than failure.
        try:
            rep = xchg(s, args.host, args.port, struct.pack("<I", TAG_FIN), TAG_AFIN,
                       max(args.timeout, 3.0), 1 if base else args.retries, args.verbose)
            _, status = struct.unpack_from("<II", rep, 0)
            if status != ST_OK:
                print(f"[logo-upload] FIN failed: {ST_NAMES.get(status, status)}"); return 4
            if base:
                print("[logo-upload] image accepted — the device is now erasing and "
                      "reprogramming its bootloader, then resetting. Do not power it off.")
            else:
                print("[logo-upload] success — device verified image and is starting the application.")
        except TimeoutError:
            if base:
                print("[logo-upload] no final ACK — the device most likely accepted the "
                      "image and is committing it. Do not power it off.")
            else:
                print("[logo-upload] no final ACK — the device likely verified the image and "
                      "already jumped to the application. Check the device (relays/behavior).")
        if base:
            # It resets into the freshly written bootloader, which finds no app
            # for the NEW partition and stays in its recovery loop. Confirm.
            print("[logo-upload] waiting for the new loader to come up...")
            if wait_for_bootloader(s, args.host, args.port, tries=60, verbose=args.verbose):
                print("[logo-upload] BASE update complete. Upload an application next.")
                return 0
            print("[logo-upload] the new loader did not answer. If it stays silent, "
                  "recover with an SD-card update.")
            return 8
        return 0
    finally:
        s.close()

if __name__ == "__main__":
    sys.exit(main())
