#!/bin/sh
# Publish web/ (AnyMouse Studio) to the public host, https://anymouse.futurelabz.dev
#   host: root@webserver.lan   directory: /opt/webserver/anymouse-webhid/
# Every change under web/ must be synced there, or the public Studio drifts from the repository
# (the dongle's wire protocol in web/proto.js in particular must match the firmware people run).
#
#   tools/deploy-web.sh        dry run: lists what would change, changes nothing
#   tools/deploy-web.sh --go   do it
#
# Not synced: tests/ and web_README.md (development files). This is an assumption about what the host
# should serve; the dry run shows exactly what differs. Nothing is ever deleted on the host.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
dest="${ANYMOUSE_WEB_DEST:-root@webserver.lan:/opt/webserver/anymouse-webhid/}"
mode=-n
[ "$1" = "--go" ] && mode=
# shellcheck disable=SC2086
exec rsync $mode -rlt --itemize-changes --no-perms --no-owner --no-group \
    --exclude tests/ --exclude web_README.md --exclude package.json \
    "$here/web/" "$dest"
