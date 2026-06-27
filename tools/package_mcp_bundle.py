#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
import platform
import shutil
import stat
import tarfile
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def copy_file(src, dst, mode=None):
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    if mode is not None:
        dst.chmod(mode)


def copy_tree(src, dst, ignore_names=()):
    if dst.exists():
        shutil.rmtree(dst)
    ignore = shutil.ignore_patterns(*ignore_names) if ignore_names else None
    shutil.copytree(src, dst, ignore=ignore)


def manifest_for(bundle_dir, bundle_name):
    files = []
    for path in sorted(p for p in bundle_dir.rglob("*") if p.is_file()):
        rel = path.relative_to(bundle_dir).as_posix()
        if rel == "manifest.json":
            continue
        files.append({
            "path": rel,
            "size": path.stat().st_size,
            "sha256": sha256(path),
            "mode": oct(stat.S_IMODE(path.stat().st_mode)),
        })
    return {
        "name": bundle_name,
        "created_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "platform": f"{platform.system().lower()}-{platform.machine()}",
        "files": files,
    }


def main():
    parser = argparse.ArgumentParser(description="Package a standalone Atari800 MCP runtime bundle.")
    parser.add_argument("--output-dir", default=str(ROOT / "dist"), help="Directory for bundle output")
    parser.add_argument("--name", default=None, help="Bundle directory/tarball base name")
    parser.add_argument("--no-tar", action="store_true", help="Create directory only, skip tar.gz")
    args = parser.parse_args()

    emulator = ROOT / "src" / "atari800"
    if not emulator.exists():
        raise SystemExit("src/atari800 does not exist; run make first")

    platform_name = f"{platform.system().lower()}-{platform.machine()}"
    bundle_name = args.name or f"atari800-mcp-{platform_name}"
    out_dir = Path(args.output_dir).resolve()
    bundle_dir = out_dir / bundle_name
    if bundle_dir.exists():
        shutil.rmtree(bundle_dir)
    bundle_dir.mkdir(parents=True)

    copy_file(emulator, bundle_dir / "bin" / "atari800", 0o755)
    copy_tree(ROOT / "mcp-server", bundle_dir / "mcp-server", ignore_names=("node_modules",))
    copy_file(ROOT / "README.AI.md", bundle_dir / "README.AI.md")
    copy_file(ROOT / "AGENT_CONTRACT.md", bundle_dir / "AGENT_CONTRACT.md")
    copy_file(ROOT / "tools" / "templates" / "start-mcp.sh", bundle_dir / "start-mcp.sh", 0o755)
    copy_file(ROOT / "tools" / "templates" / "MCP_BUNDLE_README.md", bundle_dir / "README.md")

    manifest = manifest_for(bundle_dir, bundle_name)
    (bundle_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    tar_path = None
    if not args.no_tar:
        tar_path = out_dir / f"{bundle_name}.tar.gz"
        with tarfile.open(tar_path, "w:gz") as tar:
            tar.add(bundle_dir, arcname=bundle_name)

    print(json.dumps({
        "status": "ok",
        "bundle_dir": str(bundle_dir),
        "tarball": str(tar_path) if tar_path else None,
        "manifest": str(bundle_dir / "manifest.json"),
    }, indent=2))


if __name__ == "__main__":
    main()
