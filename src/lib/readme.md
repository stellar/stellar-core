# Support libraries

This subdirectory is for storing libraries that we have integrated directly into
our build process, either due to their not being well-packaged or being small
(single file / single header) or simply for convenience while developing.

Some of these are included as part of the outer git tree. If they are
sufficiently well-packaged, or sufficiently large, they are included as git
submodules, which are configured and built in-tree, and linked statically.

Sources here should be kept as close as possible to the original form they were
acquired in, such that the code is easy to update and/or redirect to
packaged/external variants of the libraries as necessary.

Wrappers, convenience types and local customizations should be kept outside this
directory, in their own modules whenever possible.

All code here should be licensed appropriately: no more restrictive than ASL2.
The following are acceptable:

  - MIT license
  - BSD license
  - ISC license
  - Apache license 2.0
  - Boost software license
  - IBM public license
  - zlib license
