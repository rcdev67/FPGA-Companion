# stserve — your disk image collection on the ST's menu

`Download...` in the companion's menu fetches files from a web server on
your network. Any web server will do for a handful of images, and
`python3 -m http.server` is the shortest way to try it, see
[DOWNLOAD.md](../../DOWNLOAD.md).

For a real collection it is not enough. A TOSEC set is tens of thousands
of ZIP archives in folders, and the companion shows 32 entries at a time.
**stserve** sits in between:

- **Archives are opened while the file is fetched.** The ST receives the
  disk image inside, never the ZIP. Your collection stays as it is.
- **Names are cut to size.** `Arkanoid (1987)(Taito)(M4).zip` arrives on
  the SD card as `Arkanoid.ST`. Disk and version markers are kept, so
  `(Disk 2 of 2)` becomes `_d2` and `[a]` becomes `_a`.
- **Long folders are split into lettered ranges.** 5000 games become 17
  ranges of 17 ranges of about 18 games: three turns of the knob to a
  game instead of a list nothing can show.
- **Folders that hold nothing but one folder are skipped**, so the three
  levels TOSEC needs before the first game are not three menus with one
  entry each.

It reads your collection and never writes to it. Python only, no
dependencies, about 400 lines — you can read what it does.

This needs the companion of this fork from **2026-09-19** or later, the
one that can walk into folders. An older one shows the files of the top
folder only.

## On a Synology NAS

The short way, no image to build:

1. Copy `stserve.py` and `docker-compose.yml` into a folder on the NAS,
   for example `/volume1/docker/stserve` (File Station is enough).
2. Put your disk images anywhere inside a shared folder — that is what
   gives them a path like `/volume1/atari/...`, which is what a container
   can be handed. Sharing that folder with Windows or anyone else is not
   needed; stserve reads it straight off the disk.
3. Open `docker-compose.yml` and change the one line marked `<-- your
   collection` to that folder, for example
   `/volume1/atari:/games:ro`. The part before the colon is yours,
   the rest stays as it is.
4. **Container Manager → Project → Create**: give it the name `stserve`,
   pick the folder from step 1 as the path, it finds the
   `docker-compose.yml` itself, then **Next → Done**. It pulls the Python
   image once and starts.
5. In `atarist.ini` on the ST's SD card:

       server=192.168.1.10:8888

   with your NAS's address. That is the same line you would point at a PC.

`Download...` in the companion's menu now shows the collection.

To check it without the ST, open `http://192.168.1.10:8888/` in a browser
on the PC. You see the same list; `names and sizes` at the bottom of the
page shows which file of your collection is behind each name.

**Docker without Container Manager**, same thing from a shell:

    docker compose up -d

**Or build an image** from the `Dockerfile` next to it:

    docker build -t stserve .
    docker run -d --name stserve -p 8888:8888 -v /volume1/atari:/games:ro stserve

**Or no Docker at all** — on the NAS, a Raspberry Pi or the PC that has
the collection. The folder comes first, the port second:

    python3 stserve.py "D:\atari" 8889

That works the same in PowerShell, in a command prompt and in a shell.
Ctrl+C stops it. `ST_ROOT` and `ST_PORT` do the same thing for the
container, where there is no command line to put them on.

## Settings

All of them are optional; the defaults are what the companion can handle.

| Variable | Default | What it does |
|---|---|---|
| `ST_ROOT` | `/games` | the folder that is served |
| `ST_PORT` | `8888` | the port it listens on |
| `ST_MAX_ENTRIES` | `30` | entries per menu page. The companion shows at most 32 |
| `ST_MAX_BYTES` | `3400` | a page is trimmed to this; the companion reads 4 KB |
| `ST_NAME_LEN` | `30` | how long a name on the SD card may get |
| `ST_EXT` | `st,msa,dim,img,rom` | endings that count as an image |
| `ST_ZIP_EXT` | `st` | the ending an image out of an archive is offered under |
| `ST_HIDE` | — | patterns to leave out, e.g. `*[STX]*,*.txt` |
| `ST_PEEK` | `1` | look into the archives of a page and drop those without a usable image. Turn it off (`0`) for a slow disk |
| `ST_QUIET` | `0` | `1` stops the line per request in the log |

## What it will not do

- **No `.stx`, no `.ipf`.** Those hold the flux of a copy protected disk,
  and no floppy of this core can read them. A TOSEC set keeps them in
  their own folders; `ST_HIDE=*[STX]*` takes them out of the menu.
- **Nothing is written.** The collection is mounted read only on purpose.
  Save games belong on the SD card, not on the NAS.
- **No HTTPS, no user name, no password.** This is for your own network.
  Do not forward the port to the internet.
- **One image per archive.** An archive holding several disks offers the
  first image; a set of disks is several archives in TOSEC anyway.

## When something does not work

| What you see | What it is |
|---|---|
| `List is empty` | the folder holds nothing the core can read, or everything in it is hidden. `ST_HIDE` too eager, or a folder of `.stx` |
| `Modem could not fetch` | the modem did not reach the server: wrong address in `server=`, or the NAS's firewall. Try the address in a browser first |
| the menu shows folders, not files | that is the collection's own structure; walk in with the knob, `..` goes back up |
| a name shows up twice with `~2` | two files of the folder shorten to the same name. Both work |
| `permission denied` in the container log | the files belong to another user on the NAS. Put that user's id into `user:` in the compose file, or let the group read the folder |

## Check it yourself

`selftest.py` starts the server, walks down to an image the way the
companion does, fetches it and compares it byte for byte with what is
inside the archive:

    python3 selftest.py "D:\atari"

It also checks the two limits the companion cannot exceed: at most 32
entries and under 4 KB per page.
