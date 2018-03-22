#
# SYNOPSIS
#
#   AX_PKGCONFIG_SUBDIR(path/to/subproject)
#
# DESCRIPTION
#
#   Configures a project with an uninstalled.pc.in or uninstalled.pc
#   file.  If no such project is in the tree, uses PKG_CHECK_MODULES.
#
#   The problem addressed by this macro is that AC_CONFIG_SUBDIRS does
#   a lot of fancy stuff.  You definitely do not want to want to
#   invent your own version of AC_CONFIG_SUBDIRS if subdirectories
#   have their own ./configure scripts, or else you will find things
#   like out-of-directory builds breaking.  On the other hand,
#   AC_CONFIG_SUBDIRS schedules the subproject ./configure script to
#   run at the very end with AC_OUTPUT.  Hence, a
#   subproject-uninstalled.pc.in file won't get properly substituted.
#   Worse, most of these *-uninstalled.pc files are defective, because
#   for out-of-directory builds ${pcfiledir} will either be correct
#   for Libs or for Cflags but not both.
#
#   So what this macro does when a *-uninstalled.pc file exists is:
#
#     - Parse path/to/subproject/subproject-uninstalled.pc{.in,} to
#       set subproject_CFLAGS and subproject_LIBS appropriately.
#
#     - If that doesn't work, then run
#           PKG_CHECK_MODULES(subproject, subproject)
#       and return.  Otherwise...
#
#     - Set subproject_INTERNAL=yes
#
#     - Call AC_CONFIG_SUBDIRS(path/to/subproject)
#
#     - Enable automake conditional SUBPROJECT_INTERNAL
#
# LICENSE
#
# Written in 2015 by David Mazieres.  Placed in public domain.
#
AC_DEFUN([AX_PKGCONFIG_SUBDIR],
[dir="$1"
pushdef([pcname], [regexp($1,[[^/]]*$,\&)])dnl
name="pcname"
if test -f "$srcdir/$dir/$name-uninstalled.pc.in"; then
   pcfile="$srcdir/$dir/$name-uninstalled.pc.in"
   pcfiledir='$(abs_top_builddir)'
elif test -f "$srcdir/$dir/$name-uninstalled.pc"; then
   pcfile="$srcdir/$dir/$name-uninstalled.pc"
   pcfiledir='$(abs_top_srcdir)'
else
   unset pcfile
fi
if test -n "$pcfile"; then
   pcname[]_LIBS=$(sed -n \
                   -e 's|\${pcfiledir}|$(abs_top_builddir)/$1|g' \
                   -e 's/^Libs: //p' $pcfile)
   pcname[]_CFLAGS=[$(sed -n \
                     -e 's|\${pcfiledir}|'"$pcfiledir"'/$1|g' \
                     -e 's|@\([a-zA-Z][a-zA-Z0-9_]*\)@|$(\1)/$1|g' \
                     -e 's/-I/-isystem /g' \
                     -e 's/^Cflags: //p' $pcfile)]
   pcname[]_INTERNAL=yes
   AC_SUBST(pcname[]_LIBS)
   AC_SUBST(pcname[]_CFLAGS)
   AC_CONFIG_SUBDIRS($1)
   PKGCONFIG_SUBDIRS="${PKGCONFIG_SUBDIRS} \$(top_builddir)/$1"
else
   unset pcname[]_INTERNAL
   PKG_CHECK_MODULES(pcname, pcname)
fi
AM_CONDITIONAL(translit(pcname, a-z, A-Z)_INTERNAL,
               [test -n "$pcname[]_INTERNAL"])
popdef([pcname])dnl
AC_SUBST(PKGCONFIG_SUBDIRS)
])
