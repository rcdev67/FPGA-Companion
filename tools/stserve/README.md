# stserve — your game collection on the computer's menu, and in a browser

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
- **A catalogue for the browser** at `/_/`, see below: the whole
  collection with pictures, search and favourites, and a button that puts
  a game at the top of the computer's list.

It reads your collection and never writes to it. Python only, no
dependencies, one file — you can read what it does.

The same server works for the Atari ST and the C64; `docker-compose.yml`
runs one for each, see "For the C64" below.

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

**Changing an existing project**, to a new `stserve.py` or the compose file
below: stop the project in Container Manager, replace the files in its
folder, then **Action → Build** (or `docker compose up -d` in that folder).

## The catalogue in a browser

`http://<nas-ip>:8888/_/` shows the collection for a person rather than for
the modem: every game of every folder on one page, with year and publisher
from its TOSEC name, a search box, a bar of first letters, and pictures --
screenshot, title screen, box and logo where there are any.

- **Send to the computer**, in a game's view: the game is offered at the
  top of every list the companion fetches, marked `> `. On the computer:
  Shift+F12, *Download...*, *Reload list*, and it is the first entry. Once
  it has been fetched it is no longer offered.
- **The star** makes a game a favourite. Favourites are a folder of their
  own at the top of the computer's list, `Favourites/`, for the few games
  a player comes back to out of thousands; **★** in the letter bar shows
  only them in the browser.

The **pictures** are libretro's, from `thumbnails.libretro.com`, named
after the TOSEC sets. stserve fetches a picture the first time a page shows
it and keeps it in `ST_CACHE`; the first one takes a few seconds longer,
as it loads libretro's list of names (some 30 MB, kept for a month). A
game whose name matches no picture exactly is matched by title and year,
then by title alone; one that matches nothing shows its initials. The
pictures are not part of this repository, and they stay on your NAS.

Sent games and favourites are kept in `ST_CACHE` as well, so they survive
a restart. Without `ST_CACHE` the catalogue works without pictures, and
favourites last until the server stops.

## For the C64

`docker-compose.yml` has a second service, `stserve-c64`, for a C64
collection: its own container, its own folder, port **8889**, its own
cache. Change its line marked `<-- your collection`, and in `c64.ini` on
the C64's SD card:

    server=192.168.1.10:8889

What is different from the ST is all in its environment: the endings the
C64 core loads (`ST_EXT=d64,g64,prg,t64,crt,tap`), `ST_SYSTEM=Commodore -
64` for the pictures, and `ST_ZIP_EXT=*`, which names a game in an archive
after the image inside: a `.prg` arrives on the card as a `.prg`, a cartridge
as a `.crt`, and the OSD loads each the way it should.

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
| `ST_ZIP_EXT` | `st` | the ending an image out of an archive is offered under; `*` takes the ending of the image inside, for the C64 |
| `ST_HIDE` | — | patterns to leave out, e.g. `*[STX]*,*.txt` |
| `ST_PEEK` | `1` | look into the archives of a page and drop those without a usable image. Turn it off (`0`) for a slow disk |
| `ST_QUIET` | `0` | `1` stops the line per request in the log |
| `ST_CACHE` | — | a folder stserve may write to: pictures, sent games, favourites. The compose file mounts `./cache-st` and `./cache-c64` there |
| `ST_SYSTEM` | `Atari - ST` | libretro's name of the machine, for the pictures: `Atari - ST`, `Commodore - 64` |
| `ST_THUMBS` | `https://thumbnails.libretro.com` | where the pictures come from |
| `ST_TITLE` | `stserve` | the catalogue's heading |

## What it will not do

- **No `.stx`, no `.ipf`.** Those hold the flux of a copy protected disk,
  and no floppy of this core can read them. A TOSEC set keeps them in
  their own folders; `ST_HIDE=*[STX]*` takes them out of the menu.
- **Nothing is written.** The collection is mounted read only on purpose.
  Save games belong on the SD card, not on the NAS.
- **No HTTPS, no user name, no password.** This is for your own network.
  Do not forward the port to the internet. The catalogue lets anyone on
  that network send games and set favourites.
- **The pictures need the internet**, the rest does not. Without it the
  catalogue shows initials instead.
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
