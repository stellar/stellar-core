#!/bin/sh

SUBMODULES="src/lib/libsodium src/lib/xdrpp src/lib/libmedida"
GIT=`which git`

autogen_submodules()
{
	origdir=`pwd`

	submod_initialized=1
	for submod in $SUBMODULES; do
		if [ ! -f $submod/configure ]; then
			submod_initialized=0
		fi
	done

	if [ -n "$GIT" ] && [ -f .gitmodules ] && [ -d .git ] && [ $submod_initialized = 0 ]; then
        git submodule update --init
		git submodule update --init --recursive
	fi

	for submod in $SUBMODULES; do
		echo "Running autogen in '$submod'..."
		cd "$submod"
		if [ -x autogen.sh ]; then
			./autogen.sh
		elif [ -f configure.in ] || [ -f configure.ac ]; then
			autoreconf -i
		else
			echo "Don't know how to bootstrap submodule '$submod', skipping"
		fi
		cd "$origdir"
	done
}

if [ -z "$skip_submodules" ] || [ "$skip_modules" = 0 ]; then
	autogen_submodules
fi

autoreconf -v -i
