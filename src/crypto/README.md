# Crypto wrappers

This directory contains minor convenience wrappers around
[libsodium](http://libsodium.org). The fork used is stored in
[another repository](https://github.com/stellar/libsodium)
and used as a submodule by `stellar-core`.

The crypto module also contains a small implementation to turn public/private
keys into human manageable strings (StrKey).

The crypto wrappers are intended to be minimal, transparent, safe and simple;
they should not "enhance", "customize" or otherwise alter any of the
cryptographic principles or primitives provided by libsodium. Any "surprising"
behavior, for a knowledgable user of libsodium, should be considered a bug.
