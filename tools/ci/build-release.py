#!/usr/bin/env python3
"""Build every release artefact and the index that describes them.

One command so the archive, the tool binaries and the index cannot disagree
about version, filename, size or SHA-256 -- arduino-cli refuses the download
when they do, and hand-editing the index is what published a dead URL before.

  tools/ci/build-release.py --version 0.3.0 --out dist

Produces, under --out:
  autonomylogic-tm4c-<ver>.tar.bz2          the platform
  logo-upload-<ver>-<host>.tar.gz           one per host (7)
  package_autonomylogic_tm4c_index.json     regenerated to match

Uploader binaries must already be built into <out>/tools/<host>/ by
build-uploader.sh.
"""
import argparse, hashlib, json, os, shutil, subprocess, sys, tarfile

REPO = "Autonomy-Logic/logo8-arduino-core"
# What the platform actually consists of; everything else in the repo is
# repository furniture. `tools/` is deliberately absent -- the uploader now
# ships as a board-manager tool, not inside the platform archive.
PLATFORM_CONTENT = ["boards.txt", "platform.txt", "programmers.txt",
                    "cores", "libraries", "system", "variants"]
# Only the variants a board in boards.txt actually names.
KEEP_VARIANTS = {"logo8"}

HOSTS = [
    "x86_64-mingw32", "i686-mingw32",
    "x86_64-apple-darwin", "arm64-apple-darwin",
    "x86_64-pc-linux-gnu", "aarch64-linux-gnu", "arm-linux-gnueabihf",
]

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()

def meta(path, version, name):
    return {
        "url": f"https://github.com/{REPO}/releases/download/v{version}/{name}",
        "archiveFileName": name,
        "checksum": "SHA-256:" + sha256(path),
        "size": str(os.path.getsize(path)),
    }

def clean_tree(root):
    """macOS resource forks and Finder droppings must not reach an archive."""
    for dirpath, dirnames, filenames in os.walk(root):
        for n in list(dirnames):
            if n in (".git", "__pycache__"):
                shutil.rmtree(os.path.join(dirpath, n)); dirnames.remove(n)
        for n in filenames:
            if n.startswith("._") or n == ".DS_Store":
                os.remove(os.path.join(dirpath, n))

def reproducible_tar(archive, stage, root, mode):
    """Deterministic archive: sorted entries, zeroed mtime/uid/gid.

    Two builds of the same commit must produce the same SHA-256, or the index
    cannot be verified against a rebuild.
    """
    def norm(ti):
        ti.uid = ti.gid = 0
        ti.uname = ti.gname = ""
        ti.mtime = 0
        return ti
    with tarfile.open(archive, mode) as tf:
        for dirpath, dirnames, filenames in os.walk(os.path.join(stage, root)):
            dirnames.sort(); filenames.sort()
            for n in [""] + filenames:
                p = os.path.join(dirpath, n) if n else dirpath
                tf.add(p, arcname=os.path.relpath(p, stage), recursive=False, filter=norm)

def build_platform(version, out, repo_root):
    name = f"autonomylogic-tm4c-{version}.tar.bz2"
    archive = os.path.join(out, name)
    root = f"tm4c-{version}"          # arduino-cli requires one top-level dir
    stage = os.path.join(out, ".stage-platform")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(os.path.join(stage, root))
    for c in PLATFORM_CONTENT:
        src = os.path.join(repo_root, c)
        dst = os.path.join(stage, root, c)
        if os.path.isdir(src):
            shutil.copytree(src, dst)
        else:
            shutil.copy2(src, dst)
    # Drop variants no board names; they are archive weight.
    vdir = os.path.join(stage, root, "variants")
    for v in os.listdir(vdir):
        if v not in KEEP_VARIANTS:
            shutil.rmtree(os.path.join(vdir, v), ignore_errors=True)
    clean_tree(stage)
    reproducible_tar(archive, stage, root, "w:bz2")
    shutil.rmtree(stage, ignore_errors=True)
    return archive, name

def build_tool_archives(version, out):
    """Pack each prebuilt uploader into its own per-host archive."""
    systems = []
    for host in HOSTS:
        exe = "logo-upload.exe" if "mingw32" in host else "logo-upload"
        src = os.path.join(out, "tools", host, exe)
        if not os.path.exists(src):
            sys.exit(f"missing uploader for {host}: {src}\nrun tools/ci/build-uploader.sh first")
        name = f"logo-upload-{version}-{host}.tar.gz"
        archive = os.path.join(out, name)
        stage = os.path.join(out, ".stage-tool")
        shutil.rmtree(stage, ignore_errors=True)
        os.makedirs(os.path.join(stage, "logo-upload"))
        dst = os.path.join(stage, "logo-upload", exe)
        shutil.copy2(src, dst)
        os.chmod(dst, 0o755)
        reproducible_tar(archive, stage, "logo-upload", "w:gz")
        shutil.rmtree(stage, ignore_errors=True)
        systems.append({"host": host, **meta(archive, version, name)})
    return systems

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", default="dist")
    ap.add_argument("--repo-root", default=os.path.join(os.path.dirname(__file__), "..", ".."))
    args = ap.parse_args()

    repo_root = os.path.abspath(args.repo_root)
    out = os.path.abspath(args.out)
    os.makedirs(out, exist_ok=True)

    print(f"==> platform archive")
    parchive, pname = build_platform(args.version, out, repo_root)
    print(f"    {pname}  {os.path.getsize(parchive)} bytes")

    print(f"==> uploader archives")
    systems = build_tool_archives(args.version, out)
    for s in systems:
        print(f"    {s['host']:22s} {s['size']:>9s} bytes")

    print(f"==> index")
    tpl = os.path.join(repo_root, "tools", "ci", "index-template.json")
    with open(tpl) as f:
        idx = json.load(f)
    pkg = next(p for p in idx["packages"] if p["name"] == "autonomylogic")
    plat = pkg["platforms"][0]
    plat["version"] = args.version
    plat.update(meta(parchive, args.version, pname))
    plat["toolsDependencies"] = [
        {"packager": "energia", "name": "arm-none-eabi-gcc", "version": "8.3.1-20190703"},
        {"packager": "autonomylogic", "name": "logo-upload", "version": args.version},
    ]
    pkg["tools"] = [{"name": "logo-upload", "version": args.version, "systems": systems}]

    dest = os.path.join(out, "package_autonomylogic_tm4c_index.json")
    with open(dest, "w") as f:
        json.dump(idx, f, indent=2)
        f.write("\n")
    print(f"    {dest}")

if __name__ == "__main__":
    main()
