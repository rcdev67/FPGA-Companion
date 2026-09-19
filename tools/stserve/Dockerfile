# stserve - the disk image server for the MiSTeryNano's OSD download.
#
#   docker build -t stserve .
#   docker run -d --name stserve -p 8888:8888 -v /volume1/atari:/games:ro stserve
#
# On a Synology the compose file next to this one is the shorter way: it
# runs the script on the stock Python image and needs no build at all.

FROM python:3.12-alpine

COPY stserve.py /usr/local/bin/stserve.py

ENV ST_ROOT=/games \
    ST_PORT=8888

EXPOSE 8888

# The collection is only ever read, so there is no reason to be root.
# Should the files on your NAS belong to someone else, set the container's
# user to that one (see the README) or let the group read them.
USER 1000:1000

HEALTHCHECK --interval=60s --timeout=5s --start-period=5s \
  CMD python3 -c "import os,urllib.request; urllib.request.urlopen('http://127.0.0.1:'+os.environ.get('ST_PORT','8888')+'/', timeout=4)"

CMD ["python3", "/usr/local/bin/stserve.py"]
