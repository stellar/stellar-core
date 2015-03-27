# Crypto wrappers

This directory contains minor convenience wrappers around
[libsodium](http://libsodium.org), a copy of which is stored in the
[src/lib/libsodium](src/lib/libsodium) submodule of the `stellar-core`
distribution, and compiled along with it.

The crypto module also contains a small implementation of [Base58Check
encoding](https://en.bitcoin.it/wiki/Base58Check_encoding) using a custom
(non-bitcoin) dictionary.

The crypto wrappers are intended to be minimal, transparent, safe and simple;
they should not "enhance", "customize" or otherwise alter any of the
cryptographic principles or primitives provided by libsodium. Any "surprising"
behavior, for a knowledgable user of libsodium, should be considered a bug.
