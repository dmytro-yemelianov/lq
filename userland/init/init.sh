#!/bin/sh
#
# QSOE /sbin/init — first userland process after taskman.
#
# QRV / QNX-style boot: each driver is run synchronously and detaches
# itself via procmgr_detach() once it has registered its resmgr path.
# Our wait returns at that point, the driver keeps running as a daemon,
# and the next line of the script can already see /dev/ser1 in the
# pathmgr.  No backgrounding required.
#
echo "[init] starting slogger..."
/sbin/slogger

echo "[init] starting pci-server..."
/sbin/pci-server

echo "[init] starting devc-ser8250..."
/sbin/devc-ser8250

echo "[init] repointing /dev/console -> /dev/ser1..."
/sbin/repath /dev/console /dev/ser1

echo "[init] entering interactive shell..."
exec /bin/qsh -i
