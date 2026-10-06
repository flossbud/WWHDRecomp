"""The progress page's server (tools/progress/README.md): the page and its data as static files, plus the
owner's testing notes, which the page writes.

Usage (on the worker host, from the data directory): python3 server.py PORT BIND_ADDRESS

Static files are served as python's http.server serves them, but for dot files (the pid, the log, the
locks). The notes API (JSON; every request carries the header X-WWHD, so another site can't post from the
owner's browser: a custom header needs a CORS preflight, which this server never grants):
  POST   /api/notes/image   an image as the body (Content-Type image/png|jpeg|webp|gif, at most 10 MB)
                            -> {"file": "notes/img-....png"}
  POST   /api/notes         {"text", "images": [files from /api/notes/image]} -> the note
  PATCH  /api/notes/ID      {"text"?, "images"?, "done"?} -> the note
  DELETE /api/notes/ID      -> {"ok": true} (its images stay on disk)
notes.json holds them, newest first: [{id, text, images, time, edited, done, replies: [{text, session, time}]}].
Every change takes .notes.lock, which publish.sh notes (the sessions' replies) takes too.
"""
import fcntl
import http.server
import json
import os
import re
import secrets
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
NOTES = os.path.join(ROOT, "notes.json")
IMAGES = os.path.join(ROOT, "notes")
MAX_IMAGE = 10 * 1024 * 1024
MAX_TEXT = 20000
KINDS = {"image/png": (".png", b"\x89PNG\r\n\x1a\n"), "image/jpeg": (".jpg", b"\xff\xd8\xff"),
         "image/gif": (".gif", b"GIF8"), "image/webp": (".webp", b"RIFF")}


class Notes:
    """notes.json, read and written under .notes.lock (the page's writes here, the sessions' replies by ssh)."""

    def __enter__(self):
        self.lock = open(os.path.join(ROOT, ".notes.lock"), "a")
        fcntl.flock(self.lock, fcntl.LOCK_EX)
        try:
            self.list = json.load(open(NOTES)) if os.path.exists(NOTES) else []
        except ValueError:
            self.list = []
        return self

    def save(self):
        with open(NOTES + ".tmp", "w") as f:
            json.dump(self.list, f, indent=1)
        os.replace(NOTES + ".tmp", NOTES)

    def find(self, nid):
        return next((n for n in self.list if n["id"] == nid), None)

    def __exit__(self, *exc):
        fcntl.flock(self.lock, fcntl.LOCK_UN)
        self.lock.close()


def clean_images(images):
    """Only files this server stored: notes/img-*.ext that exist."""
    if not isinstance(images, list):
        return []
    out = []
    for f in images[:20]:
        if isinstance(f, str) and re.fullmatch(r"notes/img-[\w-]+\.(png|jpg|gif|webp)", f) and os.path.exists(os.path.join(ROOT, f)):
            out.append(f)
    return out


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=ROOT, **k)

    def send_head(self):
        # dot files (.pid, .server.log, the locks) aren't the page's
        if any(p.startswith(".") for p in self.path.split("?")[0].split("/") if p):
            self.send_error(404)
            return None
        return super().send_head()

    def reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def body(self, limit):
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0 or n > limit:
            return None
        return self.rfile.read(n)

    def api(self, method):
        if not self.path.startswith("/api/notes"):
            return self.reply(404, {"error": "no such thing"})
        if not self.headers.get("X-WWHD"):
            return self.reply(403, {"error": "missing X-WWHD"})
        path = self.path.split("?")[0].rstrip("/")
        if method == "POST" and path == "/api/notes/image":
            kind = (self.headers.get("Content-Type") or "").split(";")[0].strip().lower()
            if kind not in KINDS:
                return self.reply(415, {"error": "PNG, JPEG, GIF or WebP only"})
            data = self.body(MAX_IMAGE)
            if data is None:
                return self.reply(413, {"error": "empty, or over 10 MB"})
            ext, magic = KINDS[kind]
            if not data.startswith(magic) or (kind == "image/webp" and data[8:12] != b"WEBP"):
                return self.reply(415, {"error": "that isn't a " + kind})
            os.makedirs(IMAGES, exist_ok=True)
            name = f"notes/img-{time.strftime('%Y%m%d-%H%M%S')}-{secrets.token_hex(3)}{ext}"
            with open(os.path.join(ROOT, name), "wb") as f:
                f.write(data)
            return self.reply(200, {"file": name})
        raw = self.body(1024 * 1024) if method in ("POST", "PATCH") else b"{}"
        try:
            req = json.loads(raw or b"null")
            assert isinstance(req, dict)
        except (ValueError, AssertionError):
            return self.reply(400, {"error": "send a JSON object"})
        text = req.get("text")
        if text is not None and (not isinstance(text, str) or len(text) > MAX_TEXT):
            return self.reply(400, {"error": f"text: a string of at most {MAX_TEXT} characters"})
        with Notes() as notes:
            if method == "POST" and path == "/api/notes":
                text, images = (text or "").strip(), clean_images(req.get("images"))
                if not text and not images:
                    return self.reply(400, {"error": "an empty note"})
                # numbers never come back after a delete (a session may have replied to it): .notes.seq keeps the last
                seq = os.path.join(ROOT, ".notes.seq")
                last = int(open(seq).read() or 0) if os.path.exists(seq) else 0
                nid = max([last] + [int(n["id"][1:]) for n in notes.list]) + 1
                open(seq, "w").write(str(nid))
                note = {"id": f"N{nid}", "text": text, "images": images, "time": int(time.time()), "edited": None,
                        "done": False, "replies": []}
                notes.list.insert(0, note)
                notes.save()
                return self.reply(200, note)
            m = re.fullmatch(r"/api/notes/(N\d+)", path)
            note = notes.find(m.group(1)) if m else None
            if not note:
                return self.reply(404, {"error": "no such note"})
            if method == "DELETE":
                notes.list.remove(note)
                notes.save()
                return self.reply(200, {"ok": True})
            if method == "PATCH":
                if text is not None:
                    note["text"] = text.strip()
                if "images" in req:
                    note["images"] = clean_images(req["images"])
                if "done" in req:
                    note["done"] = bool(req["done"])
                if text is not None or "images" in req:
                    note["edited"] = int(time.time())
                notes.save()
                return self.reply(200, note)
        return self.reply(405, {"error": "not allowed"})

    def do_POST(self):
        self.api("POST")

    def do_PATCH(self):
        self.api("PATCH")

    def do_DELETE(self):
        self.api("DELETE")


def main():
    port, bind = int(sys.argv[1]), sys.argv[2]
    with open(os.path.join(ROOT, ".pid"), "w") as f:
        f.write(str(os.getpid()))
    http.server.ThreadingHTTPServer((bind, port), Handler).serve_forever()


if __name__ == "__main__":
    main()
