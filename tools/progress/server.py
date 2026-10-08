"""The progress page's server (tools/progress/README.md): the pages and their data as static files, plus what
the owner writes on the testing page (testing.html): bug verdicts and reports, notes, checks of new work.

Usage (on the worker's host, from the data directory): python3 server.py PORT BIND_ADDRESS

Static files are served as python's http.server serves them, but for dot files (the pid, the log, the
locks). The API (JSON; every request carries the header X-WWHD, so another site can't post from the
owner's browser: a custom header needs a CORS preflight, which this server never grants):
  POST   /api/image              an image as the body (Content-Type image/png|jpeg|webp|gif, at most 10 MB)
                                 -> {"file": "notes/img-....png"} (also at /api/notes/image)
  POST   /api/file?name=NAME     any other file as the body (at most 500 MB) -> {"file": "notes/f-.../NAME"}; served
                                 only as a download (never run as a page here), listed with a bug's or note's images
  bugs.json (under .claims.lock, as publish.sh bug takes it); the owner's notes are {text, session: "owner",
  time, images}:
  POST   /api/bugs               {"title", "details"?, "images"?} -> the new bug (open, by the owner)
  POST   /api/bugs/ID/verify     {"text"?, "images"?}  fixed and confirmed in play -> verified
  POST   /api/bugs/ID/reopen     {"text", "images"?}   still broken -> open
  POST   /api/bugs/ID/close      {"text"?}             not a bug / gone -> wontfix
  POST   /api/bugs/ID/note       {"text"?, "images"?}  a note, the state unchanged
  POST   /api/bugs/ID/retest     {}                    a verdict taken back -> fixed (to retest) again
  notes.json (general testing notes, newest first) and checks.json (the owner's verdict on finished queue
  items: {ITEM: {state: ok|problem, time, bug?}}), under .notes.lock:
  POST   /api/notes              {"text", "images"} -> the note
  PATCH  /api/notes/ID           {"text"?, "images"?, "done"?} -> the note
  DELETE /api/notes/ID           -> {"ok": true} (its images stay on disk)
  POST   /api/checks/ITEM        {"state": "ok"|"problem"|null, "bug"?} -> checks.json
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
IMAGES = os.path.join(ROOT, "notes")
MAX_IMAGE = 10 * 1024 * 1024
MAX_FILE = 500 * 1024 * 1024
MAX_TEXT = 20000
KINDS = {"image/png": (".png", b"\x89PNG\r\n\x1a\n"), "image/jpeg": (".jpg", b"\xff\xd8\xff"),
         "image/gif": (".gif", b"GIF8"), "image/webp": (".webp", b"RIFF")}


class Store:
    """A JSON file read and written under a lock file (the server's writes, and publish.sh's over ssh)."""

    def __init__(self, name, lock, empty):
        self.path, self.lockpath, self.empty = os.path.join(ROOT, name), os.path.join(ROOT, lock), empty

    def __enter__(self):
        self.lock = open(self.lockpath, "a")
        fcntl.flock(self.lock, fcntl.LOCK_EX)
        try:
            self.data = json.load(open(self.path)) if os.path.exists(self.path) else self.empty()
        except ValueError:
            self.data = self.empty()
        return self

    def save(self):
        with open(self.path + ".tmp", "w") as f:
            json.dump(self.data, f, indent=1)
        os.replace(self.path + ".tmp", self.path)

    def __exit__(self, *exc):
        fcntl.flock(self.lock, fcntl.LOCK_UN)
        self.lock.close()


def clean_images(images):
    """Only files this server stored: notes/img-*.ext and notes/f-*/NAME that exist."""
    if not isinstance(images, list):
        return []
    return [f for f in images[:20] if isinstance(f, str)
            and (re.fullmatch(r"notes/img-[\w-]+\.(png|jpg|gif|webp)", f) or re.fullmatch(r"notes/f-[\w-]+/[\w.-]+", f))
            and os.path.exists(os.path.join(ROOT, f))]


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=ROOT, **k)

    def send_head(self):
        # dot files (.pid, .server.log, the locks) aren't the page's
        if any(p.startswith(".") for p in self.path.split("?")[0].split("/") if p):
            self.send_error(404)
            return None
        return super().send_head()

    def is_upload(self):
        """A file /api/file stored: served as a download, whatever its name says it is. Decided on the file the URL
        resolves to (as the static handler resolves it), so an encoded or dotted URL can't serve one as a page."""
        f = os.path.realpath(self.translate_path(self.path))
        return f.startswith(os.path.join(os.path.realpath(IMAGES), "f-"))

    def guess_type(self, path):
        return "application/octet-stream" if self.is_upload() else super().guess_type(path)

    def end_headers(self):
        if self.command in ("GET", "HEAD") and self.is_upload():
            self.send_header("Content-Disposition", "attachment")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Content-Security-Policy", "sandbox")
        super().end_headers()

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

    def image(self):
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

    def file(self):
        from urllib.parse import parse_qs, urlsplit
        name = (parse_qs(urlsplit(self.path).query).get("name") or [""])[0]
        name = re.sub(r"[^\w.-]+", "_", os.path.basename(name)).strip("._")[:100] or "file"
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0 or n > MAX_FILE:
            return self.reply(413, {"error": "empty, or over 500 MB"})
        rel = f"notes/f-{time.strftime('%Y%m%d-%H%M%S')}-{secrets.token_hex(3)}/{name}"
        path = os.path.join(ROOT, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        # written as it arrives (never all in memory); a cut-off upload leaves nothing behind
        try:
            with open(path, "wb") as f:
                while n:
                    chunk = self.rfile.read(min(n, 1 << 20))
                    if not chunk:
                        raise ConnectionError("upload cut off")
                    f.write(chunk)
                    n -= len(chunk)
        except Exception:
            os.remove(path)
            os.rmdir(os.path.dirname(path))
            raise
        return self.reply(200, {"file": rel})

    def api(self, method):
        if not self.path.startswith("/api/"):
            return self.reply(404, {"error": "no such thing"})
        if not self.headers.get("X-WWHD"):
            return self.reply(403, {"error": "missing X-WWHD"})
        path = self.path.split("?")[0].rstrip("/")
        if method == "POST" and path in ("/api/image", "/api/notes/image"):
            return self.image()
        if method == "POST" and path == "/api/file":
            return self.file()
        raw = self.body(1024 * 1024) if method in ("POST", "PATCH") else b"{}"
        try:
            req = json.loads(raw or b"null")
            assert isinstance(req, dict)
        except (ValueError, AssertionError):
            return self.reply(400, {"error": "send a JSON object"})
        for k in ("text", "title", "details"):
            v = req.get(k)
            if v is not None and (not isinstance(v, str) or len(v) > MAX_TEXT):
                return self.reply(400, {"error": f"{k}: a string of at most {MAX_TEXT} characters"})
        now = int(time.time())
        if path.startswith("/api/bugs"):
            return self.bugs(method, path, req, now)
        if path.startswith("/api/checks/"):
            item = path[len("/api/checks/"):]
            if method != "POST" or not re.fullmatch(r"[\w-]{1,64}", item) or req.get("state") not in ("ok", "problem", None):
                return self.reply(400, {"error": "POST /api/checks/ITEM {state: ok|problem|null}"})
            with Store("checks.json", ".notes.lock", dict) as checks:
                if req["state"] is None:
                    checks.data.pop(item, None)
                else:
                    checks.data[item] = {"state": req["state"], "time": now,
                                         **({"bug": req["bug"]} if isinstance(req.get("bug"), str) else {})}
                checks.save()
                return self.reply(200, checks.data)
        if path.startswith("/api/notes"):
            return self.notes(method, path, req, now)
        return self.reply(404, {"error": "no such thing"})

    def bugs(self, method, path, req, now):
        text, images = (req.get("text") or "").strip(), clean_images(req.get("images"))
        with Store("bugs.json", ".claims.lock", list) as bugs:
            if method == "POST" and path == "/api/bugs":
                title = (req.get("title") or "").strip()
                if not title:
                    return self.reply(400, {"error": "a bug needs a title"})
                n = max([int(x["id"][1:]) for x in bugs.data] or [0]) + 1
                bug = {"id": f"B{n}", "title": title, "details": (req.get("details") or "").strip(), "state": "open",
                       "session": "", "notes": [], "reported": now, "time": now, "by": "owner", "images": images}
                bugs.data.append(bug)
                bugs.save()
                return self.reply(200, bug)
            m = re.fullmatch(r"/api/bugs/(B\d+)/(verify|reopen|close|note|retest)", path)
            bug = next((x for x in bugs.data if m and x["id"] == m.group(1)), None)
            if method != "POST" or not bug:
                return self.reply(404, {"error": "no such bug"})
            op = m.group(2)
            if op in ("reopen", "note") and not text and not images:
                return self.reply(400, {"error": "say what you saw"})
            state = {"verify": "verified", "reopen": "open", "close": "wontfix", "retest": "fixed"}.get(op)
            label = {"verify": "verified in play", "reopen": "still broken", "close": "closed", "retest": "back to retest (undone)"}.get(op)
            if state:
                bug["state"] = state
            if text or images or label:
                bug["notes"].append({"text": (f"{label}: {text}" if text else label) if label else text,
                                     "session": "owner", "time": now, **({"images": images} if images else {})})
            bug["time"] = now
            bugs.save()
            return self.reply(200, bug)

    def notes(self, method, path, req, now):
        with Store("notes.json", ".notes.lock", list) as notes:
            text = req.get("text")
            if method == "POST" and path == "/api/notes":
                text, images = (text or "").strip(), clean_images(req.get("images"))
                if not text and not images:
                    return self.reply(400, {"error": "an empty note"})
                # numbers never come back after a delete (a session may have replied to it): .notes.seq keeps the last
                seq = os.path.join(ROOT, ".notes.seq")
                last = int(open(seq).read() or 0) if os.path.exists(seq) else 0
                nid = max([last] + [int(n["id"][1:]) for n in notes.data]) + 1
                open(seq, "w").write(str(nid))
                note = {"id": f"N{nid}", "text": text, "images": images, "time": now, "edited": None, "done": False, "replies": []}
                notes.data.insert(0, note)
                notes.save()
                return self.reply(200, note)
            m = re.fullmatch(r"/api/notes/(N\d+)", path)
            note = next((n for n in notes.data if m and n["id"] == m.group(1)), None)
            if not note:
                return self.reply(404, {"error": "no such note"})
            if method == "DELETE":
                notes.data.remove(note)
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
                    note["edited"] = now
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
