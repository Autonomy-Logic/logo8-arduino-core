#!/usr/bin/env python3
"""Verify every artefact an index names: it resolves, and its size and
SHA-256 are what the index claims.

  tools/ci/verify-index.py package_autonomylogic_tm4c_index.json [--only-ours]

Exits non-zero listing every discrepancy, rather than stopping at the first,
so one run tells you everything that is wrong.
"""
import argparse, hashlib, json, sys, urllib.request

OURS = "autonomylogic"

def check(name, url, want_sha, want_size, errors):
    try:
        with urllib.request.urlopen(url, timeout=120) as r:
            data = r.read()
    except Exception as e:
        errors.append(f"{name}: cannot fetch {url} -- {e}")
        return
    got_size = len(data)
    if want_size and str(got_size) != str(want_size):
        errors.append(f"{name}: size {got_size}, index says {want_size}")
    algo, _, digest = want_sha.partition(":")
    if algo != "SHA-256":
        errors.append(f"{name}: unexpected checksum algorithm {algo!r}")
        return
    got = hashlib.sha256(data).hexdigest()
    if got.lower() != digest.lower():
        errors.append(f"{name}: sha256 {got}, index says {digest}")
    else:
        print(f"  ok  {name}  ({got_size} bytes)")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("index")
    ap.add_argument("--only-ours", action="store_true",
                    help="skip third-party packages vendored into the index")
    args = ap.parse_args()

    idx = json.load(open(args.index))
    errors = []
    for pkg in idx["packages"]:
        if args.only_ours and pkg["name"] != OURS:
            continue
        for plat in pkg.get("platforms", []):
            if not plat.get("url"):
                errors.append(f"{pkg['name']}/{plat.get('name','?')}: empty url in a published index")
                continue
            check(f"{pkg['name']} platform {plat['version']}", plat["url"],
                  plat["checksum"], plat.get("size"), errors)
        for tool in pkg.get("tools", []):
            for s in tool.get("systems", []):
                check(f"{pkg['name']} {tool['name']} {tool['version']} {s['host']}",
                      s["url"], s["checksum"], s.get("size"), errors)
    if errors:
        print("\nFAILED:", file=sys.stderr)
        for e in errors:
            print("  " + e, file=sys.stderr)
        sys.exit(1)
    print("\nindex verified")

if __name__ == "__main__":
    main()
