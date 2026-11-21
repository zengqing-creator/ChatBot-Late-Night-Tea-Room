#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Idempotent meme seeding: scan a memes directory and register every image that
is not yet in the DB, via POST /api/memes.

- Only registers images that exist on disk but are missing from GET /api/memes
  (safe to re-run; already-registered files are skipped).
- tags / description / text_on_image come from meme_manifest.json when present;
  otherwise a default tag (DEFAULT_TAGS) is used.
- Waits up to RETRY_TIMES * RETRY_INTERVAL seconds for the server to come up.

Usage:
    python scripts/seed_memes.py [base_url] [memes_dir]

Examples:
    python scripts/seed_memes.py
    python scripts/seed_memes.py http://localhost:8080 "D:/.../build/bin/Release/memes"

Stdlib only.
"""
import json
import os
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_MEMES_DIR = os.path.normpath(os.path.join(HERE, "..", "memes"))
MANIFEST = os.path.join(HERE, "meme_manifest.json")
IMAGE_EXTS = {".jpg", ".jpeg", ".png", ".gif", ".webp"}
DEFAULT_TAGS = "开心"          # fallback tag for images missing from the manifest
DEFAULT_SOURCE = "builtin"
RETRY_TIMES = 30              # wait for the server to come up
RETRY_INTERVAL = 1.0          # seconds between attempts

BASE_URL = (sys.argv[1] if len(sys.argv) > 1 else "http://localhost:8080").rstrip("/")
MEMES_DIR = os.path.normpath(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_MEMES_DIR


def http_json(url, data=None, timeout=10):
    if data is not None:
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        req = urllib.request.Request(url, data=body,
                                     headers={"Content-Type": "application/json"},
                                     method="POST")
    else:
        req = urllib.request.Request(url, method="GET")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read().decode("utf-8")
        return resp.status, (json.loads(raw) if raw else {})


def wait_for_server():
    for _ in range(RETRY_TIMES):
        try:
            http_json(BASE_URL + "/api/memes", timeout=3)
            return True
        except Exception:
            time.sleep(RETRY_INTERVAL)
    return False


def load_manifest():
    if not os.path.exists(MANIFEST):
        return {}
    with open(MANIFEST, "r", encoding="utf-8") as f:
        return {e.get("file_path", ""): e for e in json.load(f)}


def list_images(memes_dir):
    if not os.path.isdir(memes_dir):
        return []
    return sorted(f for f in os.listdir(memes_dir)
                  if os.path.splitext(f)[1].lower() in IMAGE_EXTS)


def main():
    if not wait_for_server():
        print("ERROR: cannot reach %s/api/memes (server not running?)" % BASE_URL)
        sys.exit(1)

    try:
        _, existing = http_json(BASE_URL + "/api/memes")
    except Exception as ex:
        print("ERROR: failed to read /api/memes: %s" % ex)
        sys.exit(1)
    registered = {m.get("file_path", "") for m in existing}

    images = list_images(MEMES_DIR)
    if not images:
        print("No images found in %s" % MEMES_DIR)
        return

    manifest = load_manifest()

    ok = fail = skip = 0
    for name in images:
        if name in registered:
            skip += 1
            continue
        meta = manifest.get(name, {})
        entry = {
            "file_path": name,
            "tags": meta.get("tags", DEFAULT_TAGS),
            "description": meta.get("description", ""),
            "text_on_image": meta.get("text_on_image", ""),
            "source": meta.get("source", DEFAULT_SOURCE),
        }
        try:
            status, _ = http_json(BASE_URL + "/api/memes", data=entry)
            if status == 200:
                mark = "manifest" if name in manifest else "auto"
                print("[OK] %-16s (%s) tags=%s" % (name, mark, entry["tags"]))
                ok += 1
            else:
                print("[FAIL] %s HTTP %s" % (name, status))
                fail += 1
        except Exception as ex:
            print("[FAIL] %s %s" % (name, ex))
            fail += 1

    print("Done: added %d, failed %d, skipped %d (already registered), %d on disk."
          % (ok, fail, skip, len(images)))


if __name__ == "__main__":
    main()
