#!/usr/bin/env python3
"""stserve - serve a collection of Atari ST disk images to the MiSTeryNano.

The companion's "Download..." menu fetches a directory listing over the
WiFi modem and then one file, plain HTTP. This server answers both, and
takes care of the three things that make a big collection usable on a
1980s screen through a 460800 baud wire:

  * ZIP archives are opened on the fly. A TOSEC collection keeps every
    disk image in its own archive; the ST sees the image inside, not the
    archive.
  * Names are shortened to something that fits the menu and the FAT
    directory of the SD card: "Arkanoid (1987)(Taito)(M4).zip" is served
    as "Arkanoid.ST", disk and version markers kept ("_d2", "_a").
  * A folder with more entries than the companion can show is split into
    lettered ranges, as deep as it takes. 7000 games become three
    choices of about 30.

Everything else stays deliberately plain: HTTP/1.1, always a
Content-Length, never chunked, no compression, no cookies, no redirects
except one for a missing trailing slash. That is what the modem's own
HTTP client can digest.

Nothing but the Python standard library, and the collection is only ever
read. Configuration is in environment variables, see the README.
"""

import html
import os
import posixpath
import re
import shutil
import socket
import stat
import sys
import time
import urllib.parse
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def env(name, default):
    v = os.environ.get(name)
    return default if v is None or v == "" else v


ROOT = os.path.realpath(env("ST_ROOT", "/games"))
PORT = int(env("ST_PORT", "8888"))
# The companion holds 32 entries and reads the listing into a 4 KB buffer,
# so a page stays below both.
MAX_ENTRIES = int(env("ST_MAX_ENTRIES", "30"))
MAX_BYTES = int(env("ST_MAX_BYTES", "3400"))
# The name as it lands on the SD card. The companion's own limit is 47.
NAME_LEN = int(env("ST_NAME_LEN", "30"))
# What the core can read: raw sector images, and ROM images for the record.
# .stx (Pasti) and .ipf hold flux data no FPGA floppy here can use.
IMAGE_EXT = [e.strip().lower() for e in env("ST_EXT", "st,msa,dim,img,rom").split(",") if e.strip()]
# The ending an archive's image is offered under. What is really inside
# decides when it is fetched; this only names it in the menu.
ZIP_EXT = env("ST_ZIP_EXT", "st")
# Look into the archives of a page before showing them, so that an archive
# holding nothing this core can read does not appear at all. That is one
# open per line of the menu; turn it off for a slow disk.
PEEK = env("ST_PEEK", "1") not in ("0", "no", "false")
# glob patterns, matched against the name and the path, e.g. "*[STX]*,*.txt"
HIDE = [p.strip() for p in env("ST_HIDE", "").split(",") if p.strip()]
QUIET = env("ST_QUIET", "0") not in ("0", "no", "false")

BLOCK = 64 * 1024


# --------------------------------------------------------------- names ----

# TOSEC writes "Title (1987)(Publisher)(de)(Disk 1 of 2)[cr TSB].zip". The
# title is what a player looks for; the rest only matters where it tells
# two entries apart.
_BRACKETS = re.compile(r"[\(\[][^\)\]]*[\)\]]")
_DISK = re.compile(r"\(disk (\d+)(?: of \d+)?\)", re.I)
_SIDE = re.compile(r"\(side ([ab])\)", re.I)
_ALT = re.compile(r"\[(a\d*|b\d*)\]", re.I)
_ARTICLE = re.compile(r",\s*(the|a|an|der|die|das|le|la|les|el)\s*$", re.I)
_UNSAFE = re.compile(r"[^A-Za-z0-9_-]+")


def short_name(filename, ext):
    """A short, FAT safe name for a file of the collection."""
    stem = filename.rsplit(".", 1)[0] if "." in filename else filename

    suffix = ""
    m = _DISK.search(stem)
    if m:
        suffix += "_d" + m.group(1)
    else:
        m = _SIDE.search(stem)
        if m:
            suffix += "_s" + m.group(1).upper()
    m = _ALT.search(stem)
    if m:
        suffix += "_" + m.group(1).lower()

    title = _BRACKETS.sub("", stem)
    title = _ARTICLE.sub("", title.strip())
    title = _UNSAFE.sub("_", title).strip("_")
    if not title:
        title = "FILE"

    room = NAME_LEN - len(suffix) - len(ext) - 1
    if room < 4:
        room = 4
    return title[:room].rstrip("_") + suffix + "." + ext.upper()


_PARENS = re.compile(r"\s*\([^\)]*\)")
_SQUARE = re.compile(r"\[([^\]]*)\]")
_PREFIX = re.compile(r"^atari\s*st\s*-?\s*", re.I)


def short_dir(name, parent=""):
    """A folder of the collection, without the noise every one repeats.

    TOSEC names its folders "Atari ST - Games - [ST] (TOSEC 2011-12-19)".
    Inside "Games" that is one thing: "ST". The version in brackets, the
    platform every folder here shares and the name of the folder above it
    say nothing the player did not just read.
    """
    t = _PARENS.sub("", name)            # (TOSEC-v2009-09-01_CM)
    t = _SQUARE.sub(r"\1", t)             # [ST] -> ST
    t = _PREFIX.sub("", t.strip())        # Atari ST -
    if parent:
        t = re.sub(r"^" + re.escape(parent) + r"\s*-\s*", "", t.strip(), flags=re.I)
    t = re.sub(r"\s*-\s*", "-", t.strip()).strip("-")
    t = _UNSAFE.sub("_", t).strip("_")
    if not t:
        t = _UNSAFE.sub("_", name).strip("_")
    return (t[:NAME_LEN].rstrip("_-") or "DIR")


def unique(names, name):
    """Keep served names apart, always the same way for the same folder."""
    if name.lower() not in names:
        return name
    stem, _, ext = name.rpartition(".")
    for n in range(2, 100):
        cand = "%s~%d.%s" % (stem, n, ext)
        if cand.lower() not in names:
            return cand
    return name


# -------------------------------------------------------------- entries ----


class Entry:
    """One line of a listing: a folder, a plain image, or an archive."""

    __slots__ = ("name", "real", "is_dir", "member", "size")

    def __init__(self, name, real, is_dir=False, member=None, size=0):
        self.name = name        # what the ST sees and writes to the card
        self.real = real        # the path on this machine
        self.is_dir = is_dir
        self.member = member    # the name inside the archive, if any
        self.size = size

    @property
    def href(self):
        q = urllib.parse.quote(self.name)
        return q + "/" if self.is_dir else q


def hidden(name, path):
    import fnmatch

    for p in HIDE:
        if fnmatch.fnmatch(name, p) or fnmatch.fnmatch(path, p):
            return True
    return False


# The archives of a collection never change, so what is inside one is
# worth keeping: a listing of 30 games opens 30 archives otherwise, and on
# a NAS that is 30 seeks.
_zip_cache = {}


def zip_member(path, st):
    """(member name, size, extension) of the image inside an archive."""
    key = path
    hit = _zip_cache.get(key)
    if hit and hit[0] == st.st_mtime and hit[1] == st.st_size:
        return hit[2]

    found = None
    try:
        with zipfile.ZipFile(path) as z:
            best = None
            for info in z.infolist():
                if info.is_dir():
                    continue
                ext = info.filename.rsplit(".", 1)[-1].lower() if "." in info.filename else ""
                if ext not in IMAGE_EXT:
                    continue
                rank = IMAGE_EXT.index(ext)
                if best is None or rank < best[0]:
                    best = (rank, info.filename, info.file_size, ext)
            if best:
                found = (best[1], best[2], best[3])
    except (zipfile.BadZipFile, OSError):
        found = None

    if len(_zip_cache) > 40000:
        _zip_cache.clear()
    _zip_cache[key] = (st.st_mtime, st.st_size, found)
    return found


def zip_of(entry):
    """Fill in what is inside an archive. Returns a reason when unusable."""
    try:
        st = os.stat(entry.real)
    except OSError:
        return "gone"
    m = zip_member(entry.real, st)
    if not m:
        return "no image inside"
    entry.member, entry.size = m[0], m[1]
    return ""


def listing(path):
    """Every usable entry of a folder, sorted the way the ST sees it."""
    out = []
    names = set()
    try:
        raw = sorted(os.scandir(path), key=lambda e: e.name.lower())
    except OSError:
        return out
    parent = _PREFIX.sub("", _PARENS.sub("", os.path.basename(path)).strip())

    for de in raw:
        if de.name.startswith(".") or hidden(de.name, de.path):
            continue
        try:
            st = de.stat()
        except OSError:
            continue

        if stat.S_ISDIR(st.st_mode):
            name = unique(names, short_dir(de.name, parent))
            names.add(name.lower())
            out.append(Entry(name, de.path, is_dir=True))
            continue
        if not stat.S_ISREG(st.st_mode):
            continue

        ext = de.name.rsplit(".", 1)[-1].lower() if "." in de.name else ""
        if ext == "zip":
            # What is inside is looked at when the page is rendered and when
            # the file is fetched, never for the whole folder: the name of
            # the archive is enough to sort and to find it again.
            name = unique(names, short_name(de.name, ZIP_EXT))
            names.add(name.lower())
            out.append(Entry(name, de.path, member="?", size=st.st_size))
        elif ext in IMAGE_EXT:
            name = unique(names, short_name(de.name, ext))
            names.add(name.lower())
            out.append(Entry(name, de.path, size=st.st_size))

    # The menu is sorted by what it shows, not by what is on the disk:
    # "'Nam 1965-1975 (1991).zip" is filed under N, where a player looks
    # for it, not in front of everything else.
    out.sort(key=lambda e: (not e.is_dir, e.name.lower()))
    return out


def listing_of(path, depth=8):
    """A folder's listing, walking through folders that hold only a folder.

    A collection like TOSEC nests "Games / ST / Atari ST - Games - [ST]
    (TOSEC ...)" before the first game shows up. Those are three menus with
    nothing to choose, so they are skipped: the ST sees the games right
    away, and the path it walks keeps one step per choice it made.
    """
    entries = listing(path)
    while depth > 0 and len(entries) == 1 and entries[0].is_dir:
        path = entries[0].real
        entries = listing(path)
        depth -= 1
    return path, entries


# -------------------------------------------------------------- buckets ----


def _stem(name):
    return name.rsplit(".", 1)[0] if "." in name else name


def _key(name, depth):
    """The drawer a name belongs in: its first letter, then its first two."""
    return _stem(name)[:depth].upper()


def _merge(groups, limit):
    """Put neighbouring drawers together until they fit on one page.

    A collection has few games under X and many under S. Merging the small
    drawers keeps the page short without tearing the big ones apart, and
    what comes out reads like the spine of a card index: 1-9, A, B, C.
    """
    total = sum(len(g) for _, g in groups)
    target = max(1, -(-total // limit))
    while True:
        bins, cur, cur_n = [], [], 0
        for key, g in groups:
            if cur and cur_n + len(g) > target:
                bins.append(cur)
                cur, cur_n = [], 0
            cur.append((key, g))
            cur_n += len(g)
        if cur:
            bins.append(cur)
        if len(bins) <= limit or target >= total:
            return bins
        target = int(target * 1.3) + 1


def buckets(entries, depth=1):
    """File a long listing into drawers by first letter.

    Level one is the first letter, level two the first two, and so on as
    deep as a collection needs. Everything is computed from the folder's
    contents alone, so the drawers are the same on the request that shows
    them and on the request that walks into one.
    """
    groups = []
    for e in entries:
        k = _key(e.name, depth)
        if groups and groups[-1][0] == k:
            groups[-1][1].append(e)
        else:
            groups.append((k, [e]))

    # Names that are the same this far down: deeper drawers would all hold
    # the same thing, so they are cut into equal pieces instead.
    if len(groups) == 1 and len(entries) > MAX_ENTRIES and depth > 12:
        size = -(-len(entries) // MAX_ENTRIES)
        return [("%d" % (i // size + 1), entries[i:i + size])
                for i in range(0, len(entries), size)]

    labels = set()
    out = []
    for b in _merge(groups, MAX_ENTRIES):
        chunk = [e for _, g in b for e in g]
        # the drawer's name in the spelling of the folder, "As", not "AS"
        first = _stem(b[0][1][0].name)[:depth]
        last = _stem(b[-1][1][-1].name)[:depth]
        first = first[:1].upper() + first[1:]
        last = last[:1].upper() + last[1:]
        label = first if first.upper() == last.upper() else "%s-%s" % (first, last)
        if label.lower() in labels:
            for k in range(2, 100):
                cand = "%s~%d" % (label, k)
                if cand.lower() not in labels:
                    label = cand
                    break
        labels.add(label.lower())
        out.append((label, chunk))
    return out


# ------------------------------------------------------------ the server ----


class Resolved:
    def __init__(self, kind, path=None, entries=None, entry=None, depth=1):
        self.kind = kind            # "dir", "file" or None
        self.path = path
        self.entries = entries
        self.entry = entry
        self.depth = depth          # how many drawers deep inside a folder


def resolve(parts):
    """Walk a request path over folders, lettered ranges and names."""
    cur, entries = listing_of(ROOT)
    depth = 1

    for part in parts:
        if part in ("", ".", ".."):
            return Resolved(None)

        # a drawer of this folder, not a folder of its own
        if len(entries) > MAX_ENTRIES:
            hit = None
            for lab, chunk in buckets(entries, depth):
                if lab.lower() == part.lower():
                    hit = chunk
                    break
            if hit is not None:
                entries = hit
                depth += 1
                continue

        for e in entries:
            if e.name.lower() == part.lower():
                if e.is_dir:
                    cur, entries = listing_of(e.real)
                    depth = 1
                    break
                return Resolved("file", entry=e)
        else:
            return Resolved(None)

    return Resolved("dir", path=cur, entries=entries, depth=depth)


def page(url_path, entries, depth, full):
    """The listing. Kept small: the companion reads it into 4 KB."""
    shown = list(entries)
    ranges = []
    if len(shown) > MAX_ENTRIES:
        ranges = buckets(shown, depth)
        shown = []

    title = html.escape(url_path or "/")
    head = ("<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
            "<title>%s</title></head><body><h1>%s</h1><ul>" % (title, title))
    if url_path:
        head += "<li><a href=\"../\">../</a></li>"

    lines = []
    for lab, chunk in ranges:
        lines.append("<li><a href=\"%s/\">%s/</a> <small>%d</small></li>"
                     % (urllib.parse.quote(lab), html.escape(lab), len(chunk)))
    for e in shown:
        if PEEK and e.member and not e.is_dir and zip_of(e):
            continue                          # an archive without a usable image
        line = "<li><a href=\"%s\">%s</a>" % (e.href, html.escape(e.name))
        if full and not e.is_dir and (not e.member or not zip_of(e)):
            line += " <small>%s%s, %d KB</small>" % (
                html.escape(os.path.basename(e.real)),
                " : " + html.escape(e.member) if e.member else "",
                (e.size + 1023) // 1024)
        lines.append(line + "</li>")

    # Trim rather than overrun the companion's buffer: a listing that got
    # cut in the middle of a link would lose the entry silently.
    if not full:
        room = MAX_BYTES - len(head) - 32
        while lines and sum(len(l) for l in lines) > room:
            lines.pop()

    tail = "</ul>"
    if not full:
        tail += "<p><a href=\"?full=1\">names and sizes</a></p>"
    return (head + "".join(lines) + tail + "</body></html>").encode("utf-8", "replace")


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "stserve/1.0"

    def log_message(self, fmt, *args):
        if not QUIET:
            sys.stdout.write("%s %s\n" % (self.address_string(), fmt % args))
            sys.stdout.flush()

    def log_error(self, fmt, *args):
        # a transfer the ST broke off is not worth a stack trace
        self.log_message(fmt, *args)

    def fail(self, code, text):
        body = ("%d %s\r\n" % (code, text)).encode()
        self.send_response(code, text)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)
        self.close_connection = True

    def do_HEAD(self):
        self.serve(False)

    def do_GET(self):
        self.serve(True)

    def serve(self, body):
        url = urllib.parse.urlsplit(self.path)
        full = "full" in urllib.parse.parse_qs(url.query)
        path = urllib.parse.unquote(url.path)
        parts = [p for p in path.split("/") if p]
        if any(p in (".", "..") for p in parts):
            return self.fail(403, "Forbidden")

        r = resolve(parts)
        if r.kind is None:
            return self.fail(404, "Not Found")

        if r.kind == "dir":
            if path and not path.endswith("/"):
                # so that "../" in the listing means what it says
                self.send_response(301, "Moved Permanently")
                self.send_header("Location", urllib.parse.quote(path) + "/")
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                self.close_connection = True
                return
            data = page("/".join(parts), r.entries, r.depth, full)
            self.send_response(200, "OK")
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Connection", "close")
            self.end_headers()
            if body:
                self.wfile.write(data)
            self.close_connection = True
            return

        self.send_image(r.entry, body)

    def send_image(self, e, body):
        if e.member and zip_of(e):
            self.log_message("no image inside %s", e.real)
            return self.fail(404, "Not Found")
        try:
            if e.member:
                zf = zipfile.ZipFile(e.real)
                info = zf.getinfo(e.member)
                size, src = info.file_size, zf.open(info)
            else:
                size, src, zf = os.path.getsize(e.real), open(e.real, "rb"), None
        except (OSError, KeyError, zipfile.BadZipFile) as exc:
            self.log_message("cannot open %s: %s", e.real, exc)
            return self.fail(404, "Not Found")

        try:
            self.send_response(200, "OK")
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(size))
            self.send_header("Connection", "close")
            self.end_headers()
            if body:
                shutil.copyfileobj(src, self.wfile, BLOCK)
        except (BrokenPipeError, ConnectionResetError, socket.timeout):
            self.log_message("transfer of %s broke off", e.name)
        finally:
            src.close()
            if zf:
                zf.close()
            self.close_connection = True


class Server(ThreadingHTTPServer):
    daemon_threads = True
    # The modem opens a connection per file and drops it when the ST
    # cancels; a half open one must not keep a thread for good.
    timeout = 30


USAGE = """stserve - serve a collection of Atari ST disk images

    stserve.py [folder] [port]

The folder holds the images, the port defaults to 8888. Both can come
from ST_ROOT and ST_PORT instead, which is what the container does; the
arguments win. Every setting is in the README.
"""


def main(argv):
    global ROOT, PORT

    if "-h" in argv or "--help" in argv or "/?" in argv:
        sys.stdout.write(USAGE)
        return 0

    args = [a for a in argv[1:] if not a.startswith("-")]
    if args:
        ROOT = os.path.realpath(args[0])
    if len(args) > 1:
        try:
            PORT = int(args[1])
        except ValueError:
            sys.stderr.write("stserve: '%s' is not a port number\n" % args[1])
            return 1

    if not os.path.isdir(ROOT):
        sys.stderr.write("stserve: '%s' is not a folder\n\n%s" % (ROOT, USAGE))
        return 1

    n = len(listing_of(ROOT)[1])
    sys.stdout.write(
        "stserve on port %d, serving %s (%d entries at the top, %d per page)\n"
        % (PORT, ROOT, n, MAX_ENTRIES))
    sys.stdout.flush()
    try:
        Server(("", PORT), Handler).serve_forever()
    except KeyboardInterrupt:
        sys.stdout.write("stserve stopped\n")
    except OSError as e:
        sys.stderr.write("stserve: cannot listen on port %d: %s\n" % (PORT, e))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
