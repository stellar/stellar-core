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

case "${skip_submodules}" in
    0|no|false|"")
        git submodule update --init
        git submodule foreach 'test ! -x ./autogen.sh || ./autogen.sh'
    ;;
esac

./make-mks
autoreconf -i
