#!/bin/sh -e

# Public domain

case "$0" in
    */*)
	cd $(dirname $0)
	;;
esac

case "$1" in
    --skip-submodules|-s)
	skip_submodules=yes
	;;
    "")
	;;
    *)
	echo usage: $0 [--skip-submodules] >&2
	exit 1
	;;
esac

# NB: the DO_NOT_UPDATE_CONFIG_SCRIPTS variable is here to inform libsodium not
# to download a fresh config.sub and config.guess from git.savannah.gnu.org
# (which is sometimes offline).
#
# This variable was how you disable the update in libsodium up to version
# 1.0.18; but in master they have changed this, so on the next libsodium
# submodule bump we'll want to change the code here to the new interface
# (running `autogen.sh -b` in the libsodium directory)

case "${skip_submodules}" in
    0|no|false|"")
        git submodule update --init
        git submodule foreach '
            autogen=$(find . -name autogen.sh)
            if [ -x "$autogen" ]; then
                cd $(dirname "$autogen")
                DO_NOT_UPDATE_CONFIG_SCRIPTS=1 ./autogen.sh
            fi
            '
    ;;
esac

./make-mks
autoreconf -i
