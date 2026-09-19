#!/usr/bin/env python3
"""Check stserve against a real collection, without an ST in the room.

    ST_ROOT="/volume1/atari" python3 selftest.py

It starts the server on a free port, walks into the collection the way
the companion does, downloads one image and compares it byte for byte
with what is inside the archive. It also checks the two limits the
companion cannot exceed: 32 entries and a 4 KB listing.
"""

import hashlib
import io
import os
import re
import sys
import threading
import urllib.request
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import stserve                                              # noqa: E402

HREF = re.compile(r'href="([^"]*)"')
FAILED = []


def check(ok, what, detail=""):
    print("%-4s %s%s" % ("ok" if ok else "FAIL", what, (" - " + detail) if detail else ""))
    if not ok:
        FAILED.append(what)
    return ok


def get(base, path):
    """Fetch like the modem does: plain HTTP/1.1, connection closed."""
    req = urllib.request.Request(base + path, headers={"User-Agent": "Zimodem Firmware"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.status, dict(r.headers), r.read()


def entries(body):
    """The links the companion would pick out of a listing."""
    out = []
    for href in HREF.findall(body.decode("utf-8", "replace")):
        if href.startswith("?") or href == "../":
            continue
        if href.count("/") > 1 or (href.count("/") == 1 and not href.endswith("/")):
            continue
        out.append(href)
    return out


def main():
    port = 8000
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    stserve.PORT = port

    srv = stserve.Server(("127.0.0.1", port), stserve.Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d" % port
    print("stserve on %s, root %s\n" % (base, stserve.ROOT))

    # --- the way down to a single image, the way the companion walks it
    path = "/"
    depth = 0
    last_file = None
    while depth < 12:
        status, head, body = get(base, path)
        if not check(status == 200, "listing %s" % path, "status %d" % status):
            break
        check(len(body) < 4096, "listing fits the companion's buffer",
              "%d bytes" % len(body))
        links = entries(body)
        check(0 < len(links) <= 32, "listing has 1..32 entries at %s" % path,
              "%d entries" % len(links))
        if not links:
            break
        files = [l for l in links if not l.endswith("/")]
        dirs = [l for l in links if l.endswith("/")]
        if files:
            last_file = path + files[0]
            print("     %d folders, %d files, first file %s" % (len(dirs), len(files), files[0]))
            break
        print("     %d folders, entering %s" % (len(dirs), dirs[0]))
        path = path + dirs[0]
        depth += 1

    if not check(last_file is not None, "found an image by walking down"):
        return 1
    print()

    # --- the download itself
    status, head, body = get(base, last_file)
    check(status == 200, "download %s" % last_file, "status %d" % status)
    clen = head.get("Content-Length")
    check(clen is not None, "Content-Length is there (the modem needs it)",
          str(clen))
    check(head.get("Transfer-Encoding") is None, "not chunked")
    check(clen is not None and int(clen) == len(body),
          "Content-Length matches the body", "%s vs %d" % (clen, len(body)))
    check(len(body) % 512 == 0, "size is a whole number of sectors",
          "%d bytes = %d sectors" % (len(body), len(body) // 512))

    # --- and the same bytes as in the archive
    name = urllib.parse.unquote(last_file.rsplit("/", 1)[-1])
    r = stserve.resolve([urllib.parse.unquote(p) for p in last_file.split("/") if p])
    if check(r.kind == "file", "the server resolves the name to a file"):
        if r.entry.member:
            check(stserve.zip_of(r.entry) == "", "the archive holds a usable image")
            with zipfile.ZipFile(r.entry.real) as z:
                want = z.read(r.entry.member)
            check(hashlib.md5(want).hexdigest() == hashlib.md5(body).hexdigest(),
                  "unpacked image is identical to the archive member",
                  "%s : %s" % (os.path.basename(r.entry.real), r.entry.member))
        else:
            with open(r.entry.real, "rb") as f:
                want = f.read()
            check(want == body, "served file is identical")

    # --- the things that must not work
    status = 0
    try:
        status, _, _ = get(base, "/../stserve.py")
    except urllib.error.HTTPError as e:
        status = e.code
    check(status in (403, 404), "a path out of the root is refused", "status %d" % status)
    try:
        status, _, _ = get(base, "/nothing-here.ST")
    except urllib.error.HTTPError as e:
        status = e.code
    check(status == 404, "an unknown name gives 404", "status %d" % status)

    # --- the human view
    status, _, body = get(base, "/?full=1")
    check(status == 200 and b"full" not in body.split(b"<ul>")[1][:20],
          "the browser view with names and sizes answers", "status %d" % status)

    print()
    if FAILED:
        print("%d check(s) failed: %s" % (len(FAILED), ", ".join(FAILED)))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    import urllib.error
    import urllib.parse
    sys.exit(main())
