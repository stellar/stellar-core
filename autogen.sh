#!/bin/sh

# Directory check.
if [ ! -f autogen.sh ]; then
  echo "Run ./autogen.sh from the directory it exists in."
  exit 1
fi

run()
{
  echo "Running $1 ..."
  $1
}

AUTORECONF=${AUTORECONF:-autoreconf}

run "$AUTORECONF"

