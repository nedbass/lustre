#
# LB_CHECK_VERSION
#
# Verify that LUSTRE_VERSION was defined properly
#
AC_DEFUN([LB_CHECK_VERSION],
[if test "LUSTRE_VERSION" = "LUSTRE""_VERSION" ; then
	AC_MSG_ERROR([This script was not built with a version number.])
fi
])

#
# LB_CANONICAL_SYSTEM
#
# fixup $target_os for use in other places
#
AC_DEFUN([LB_CANONICAL_SYSTEM],
[case $target_os in
	linux*)
		lb_target_os="linux"
		;;
	darwin*)
		lb_target_os="darwin"
		;;
	solaris*)
		lb_target_os="SunOS"
		;;esac
AC_SUBST(lb_target_os)
])

#
# LB_DOWNSTREAM_RELEASE
#
AC_DEFUN([LB_DOWNSTREAM_RELEASE],
[AC_ARG_WITH([downstream-release],
	AC_HELP_STRING([--with-downstream-release=string],
		       [set a string in the BUILD_VERSION and RPM Release: (default is nothing)]),
	[DOWNSTREAM_RELEASE=$with_downstream_release],
	[
	# if not specified, see if it's in the META file
	if test -f META; then
		DOWNSTREAM_RELEASE=$(sed -ne '/^LOCAL_VERSION =/s/.*= *//p' META)
	fi
	])
AC_SUBST(DOWNSTREAM_RELEASE)
])

#
# LB_BUILDID
#
# Check if the source is a GA release and if not, set a "BUILDID"
#
# Currently there are at least two ways/modes of/for doing this.  One
# is if we are in a valid git repository, the other is if we are in a
# non-git source tree of some form.  Building the latter from the former
# will be handled here.
AC_DEFUN([LB_BUILDID],
[
AC_MSG_CHECKING([for buildid])
BUILDID=""
if git branch >/dev/null 2>&1; then
	ffw=0
	hash=""
	ver=$(git describe --match v[[0-9]]_*_[[0-9]] --tags)
	if [[[ $ver = *-*-* ]]]; then
		hash=${ver##*-}
		ffw=${ver#*-}
		ffw=${ffw%-*}
		ver=${ver%%-*}
	fi
	# it's tempting to use [[ $ver =~ ^v([0-9]+_)+([0-9]+|RC[0-9]+)$ ]]
	# here but the portability of the regex on the right is dismal
	# (thanx suse)
	if echo "$ver" | egrep -q "^v([0-9]+_)+([0-9]+|RC[0-9]+)$"; then
		ver=$(echo $ver | sed -e 's/^v\(.*\)/\1/' \
				      -e 's/_RC[[0-9]].*$//' -e 's/_/./g')
	fi

	# a "lustre fix" value of .0 should be truncated
	if [[[ $ver = *.*.*.0 ]]]; then
		ver=${ver%.0}
	fi
	# ditto for a "lustre fix" value of _0
	if [[[ $ver = v*_*_*_0 ]]]; then
		ver=${ver%_0}
	fi
	if [[[ $ver = v*_*_* ]]]; then
		ver=${ver#v}
		ver=${ver//_/.}
	fi

	# only do this test for lustre (not ldiskfs)
	if test "$PACKAGE" = "lustre" -a "$ver" != "$VERSION"; then
		AC_MSG_WARN([most recent tag found: $ver does not match current version $VERSION.])
	fi

	if test "$ffw" != "0"; then
		BUILDID="$hash"
		msg="$BUILDID (ahead by $ffw commits)"
		AC_MSG_RESULT([$msg])
	else
		AC_MSG_RESULT([none... congratulations, you must be on a tag])
	fi
elif test -f META; then
	BUILDID=$(sed -ne '/^BUILDID =/s/.*= *//p' META)
	msg="$BUILDID (from META file)"
	AC_MSG_RESULT([$msg])
else
	AC_MSG_WARN([FIXME: I don't know how to deal with source trees outside of git that don't have a META file.  Not setting a buildid.])
fi
AC_SUBST(BUILDID)
])

#
# LB_CHECK_FILE
#
# Check for file existance even when cross compiling
#
AC_DEFUN([LB_CHECK_FILE],
[AS_VAR_PUSHDEF([lb_File], [lb_cv_file_$1])dnl
AC_CACHE_CHECK([for $1], lb_File,
[if test -r "$1"; then
  AS_VAR_SET(lb_File, yes)
else
  AS_VAR_SET(lb_File, no)
fi])
AS_IF([test AS_VAR_GET(lb_File) = yes], [$2], [$3])[]dnl
AS_VAR_POPDEF([lb_File])dnl
])# LB_CHECK_FILE


#
# LB_ARG_LIBS_INCLUDES
#
# support for --with-foo, --with-foo-includes, and --with-foo-libs in
# a single magical macro
#
AC_DEFUN([LB_ARG_LIBS_INCLUDES],
[lb_pathvar="m4_bpatsubst([$2], -, _)"
AC_MSG_CHECKING([for $1])
AC_ARG_WITH([$2],
	AC_HELP_STRING([--with-$2=path],
		[path to $1]),
	[],[withval=$4])

if test x$withval = xyes ; then
	eval "$lb_pathvar='$3'"
else
	eval "$lb_pathvar='$withval'"
fi
AC_MSG_RESULT([${!lb_pathvar:-no}])

if test x${!lb_pathvar} != x -a x${!lb_pathvar} != xno ; then
	AC_MSG_CHECKING([for $1 includes])
	AC_ARG_WITH([$2-includes],
		AC_HELP_STRING([--with-$2-includes=path],
			[path to $1 includes]),
		[],[withval='yes'])

	lb_includevar="${lb_pathvar}_includes"
	if test x$withval = xyes ; then
		eval "${lb_includevar}='${!lb_pathvar}/include'"
	else
		eval "${lb_includevar}='$withval'"
	fi
	AC_MSG_RESULT([${!lb_includevar}])

	AC_MSG_CHECKING([for $1 libs])
	AC_ARG_WITH([$2-libs],
		AC_HELP_STRING([--with-$2-libs=path],
			[path to $1 libs]),
		[],[withval='yes'])

	lb_libvar="${lb_pathvar}_libs"
	if test x$withval = xyes ; then
		eval "${lb_libvar}='${!lb_pathvar}/lib'"
	else
		eval "${lb_libvar}='$withval'"
	fi
	AC_MSG_RESULT([${!lb_libvar}])
fi
])
])

#
# LB_PATH_LIBSYSIO
#
# Handle internal/external libsysio
#
AC_DEFUN([LB_PATH_LIBSYSIO],
[AC_ARG_WITH([sysio],
	AC_HELP_STRING([--with-sysio=path],
			[set path to libsysio source (default is included libsysio)]),
	[],[
		case $lb_target_os in
			linux)
				with_sysio='yes'
				;;
			*)
				with_sysio='no'
				;;
		esac
	])
AC_MSG_CHECKING([location of libsysio])
enable_sysio="$with_sysio"
case x$with_sysio in
	xyes)
		AC_MSG_RESULT([internal])
		LB_CHECK_FILE([$srcdir/libsysio/src/rmdir.c],[],[
			AC_MSG_ERROR([A complete internal libsysio was not found.])
		])
		LIBSYSIO_SUBDIR="libsysio"
		SYSIO="$PWD/libsysio"
		;;
	xno)
		AC_MSG_RESULT([disabled])
		;;
	*)
		AC_MSG_RESULT([$with_sysio])
		LB_CHECK_FILE([$with_sysio/lib/libsysio.a],[],[
			AC_MSG_ERROR([A complete (built) external libsysio was not found.])
		])
		SYSIO=$with_sysio
		with_sysio="yes"
		;;
esac

# We have to configure even if we don't build here for make dist to work
AC_CONFIG_SUBDIRS(libsysio)
])

#
# LB_PATH_LUSTREIOKIT
#
# Handle internal/external lustre-iokit
#
AC_DEFUN([LB_PATH_LUSTREIOKIT],
[AC_ARG_WITH([lustre-iokit],
	AC_HELP_STRING([--with-lustre-iokit=path],
			[set path to lustre-iokit source (default is included lustre-iokit)]),
	[],[
			with_lustre_iokit='yes'
	])
AC_MSG_CHECKING([location of lustre-iokit])
enable_lustre_iokit="$with_lustre_iokit"
case x$with_lustre_iokit in
	xyes)
		AC_MSG_RESULT([internal])
		LB_CHECK_FILE([$srcdir/lustre-iokit/ior-survey/ior-survey],[],[
			AC_MSG_ERROR([A complete internal lustre-iokit was not found.])
		])
		LUSTREIOKIT_SUBDIR="lustre-iokit"
		LUSTREIOKIT="$PWD/lustre-iokit"
		;;
	xno)
		AC_MSG_RESULT([disabled])
		;;
	*)
		AC_MSG_RESULT([$with_lustre_iokit])
		LB_CHECK_FILE([$with_lustre_iokit/ior-survey/ior_survey],[],[
			AC_MSG_ERROR([A complete (built) external lustre-iokit was not found.])
		])
		LUSTREIOKIT="$with_lustre_iokit"
		;;
esac
AC_SUBST(LUSTREIOKIT_SUBDIR)
# We have to configure even if we don't build here for make dist to work
AC_CONFIG_SUBDIRS(lustre-iokit)
])

#
# LB_CONFIG_CRAY_XT3
#
# Enable Cray XT3 features
#
AC_DEFUN([LB_CONFIG_CRAY_XT3],
[AC_MSG_CHECKING([whether to build Cray XT3 features])
AC_ARG_ENABLE([cray_xt3],
	AC_HELP_STRING([--enable-cray-xt3],
			[enable building of Cray XT3 features]),
	[enable_cray_xt3='yes'],[enable_cray_xt3='no'])
AC_MSG_RESULT([$enable_cray_xt3])
if test x$enable_cray_xt3 != xno; then
        AC_DEFINE(CRAY_XT3, 1, Enable Cray XT3 Features)
fi
])

#
# LB_CONFIG_BGL
#
# Enable BGL features
#
AC_DEFUN([LB_CONFIG_BGL],
[AC_MSG_CHECKING([whether to build BGL features])
AC_ARG_ENABLE([bgl],
	AC_HELP_STRING([--enable-bgl],
			[enable building of BGL features]),
	[enable_bgl='yes'],[enable_bgl='no'])
AC_MSG_RESULT([$enable_bgl])
if test x$enable_bgl != xno; then
        AC_DEFINE(HAVE_BGL_SUPPORT, 1, Enable BGL Features)
        enable_doc='no'
        enable_tests='no'
        enable_server='no'
        enable_liblustre='no'
        enable_libreadline='no'
fi
])

# Optionally configure/make the spl sources.  If the sources are
# determined to reside in-tree this feature will automatically be
# enabled.  If the sources are not in-tree it will be disabled.
# Use --enable-spl-build or --disable-spl-build if you need to
# override this behavior.
AC_DEFUN([LB_SPL_BUILD],
[AC_ARG_ENABLE([spl-build],
	AC_HELP_STRING([--enable-spl-build], [enable spl configure/make]),
	[], [
		SPL_DIR_INTREE=$(readlink -f $PWD/spl)
		if test x$SPL_DIR = x$SPL_DIR_INTREE; then
			enable_spl_build='yes'
		else
			enable_spl_build='no'
		fi
	])

AC_MSG_CHECKING([whether to build spl])
if test x$enable_spl_build = xyes; then
	AC_MSG_RESULT([$enable_spl_build])

	LB_CHECK_FILE([$SPL_DIR/module/spl/spl-generic.c], [], [
		AC_MSG_ERROR([Complete spl source must exist when building.])])

	LB_CHECK_FILE([$SPL_DIR/configure], [], [
		AC_MSG_ERROR([Complete spl source must exist when building.])])

	SPL_SUBDIR="$SPL_DIR"
	AC_SUBST(SPL_SUBDIR)
	AC_CONFIG_SUBDIRS("spl")
else
	enable_spl_build='no'
	AC_MSG_RESULT([$enable_spl_build])
fi
])

AC_DEFUN([LB_SPL_SYMVERS],
[AC_MSG_CHECKING([spl module symbols])
if test -r $SPL_OBJ/Module.symvers; then
	SPL_SYMBOLS=Module.symvers
elif test -r $SPL_OBJ/Modules.symvers; then
	SPL_SYMBOLS=Modules.symvers
elif test -r $SPL_OBJ/module/Module.symvers; then
	SPL_SYMBOLS=Module.symvers
elif test -r $SPL_OBJ/module/Modules.symvers; then
	SPL_SYMBOLS=Modules.symvers
else
	SPL_SYMBOLS=$SYMVERFILE
fi

AC_MSG_RESULT([$SPL_SYMBOLS])
AC_SUBST(SPL_SYMBOLS)
])

AC_DEFUN([LB_SPL_RELEASE],
[AC_MSG_CHECKING([spl source release])
if test -r $SPL_OBJ/spl_config.h; then
	tmp_flags="$EXTRA_KCFLAGS"
	EXTRA_KCFLAGS="-I$SPL_DIR $EXTRA_KCFLAGS"
	LB_LINUX_TRY_MAKE([
		#include <$SPL_OBJ/spl_config.h>
	],[
		char *SPL_RELEASE;
		SPL_RELEASE=SPL_META_VERSION;
	],[
		$makerule LUSTRE_KERNEL_TEST=conftest.i
	],[
		test -s build/conftest.i
	],[
		eval $(grep "SPL_RELEASE=" build/conftest.i)
	],[
		AC_MSG_RESULT([unknown])
		AC_MSG_ERROR([Could not preprocess test program.])
	])
	EXTRA_KCFLAGS="$tmp_flags"
	rm build/conftest.i
elif test -r $SPL_DIR/META; then
	SPL_RELEASE=$(awk '/Version/ { print [$]2 }' $SPL_DIR/META)
else
	AC_MSG_RESULT([unknown])
	AC_MSG_ERROR([Could not locate spl_config.h or META to check release.])
fi

if test x$SPL_RELEASE = x; then
	AC_MSG_RESULT([unknown])
	AC_MSG_ERROR([Could not determine spl release.])
fi

AC_MSG_RESULT([$SPL_RELEASE])
AC_SUBST(SPL_RELEASE)
])

#
# LB_PATH_SPL
# --with-spl      - Enable spl support and attempt to autodetect the spl
# --with-spl=yes    headers in one of the following places in this order:
#                   * ./spl
#                   * /usr/src/spl-*/$LINUXRELEASE
#                   * ../spl
#
# --with-spl=path - Enable spl support and use the spl headers in the
#                   provided path.  No autodetection is performed.
#
# --with-spl-obj  - When spl support is enabled the object directory
#                   will be based on the --with-spl directory.  If this
#                   is detected incorrectly it can be explicitly
#                   specified using this option.
#
# NOTE: As with all external packages spl is expected to already be
# configured and built.  However, if the spl tree is located in-tree
# (./spl) then it will be configured and built recursively as part of
# the lustre build system.
#
AC_DEFUN([LB_PATH_SPL],
[AC_ARG_WITH([spl],
	AC_HELP_STRING([--with-spl=path], [set path to spl sources]),
	[],[
		if test x$linux25$enable_server = xyesyes; then
			with_spl='yes'
		else
			with_spl='no'
		fi
	])

case x$with_spl in
	xno)
		AC_MSG_ERROR([spl must be enabled when building zfs.])
		;;
	xyes)
		SPL_DIR=

		# Check ./spl
		spl_src=$PWD/spl
		if test -e "$spl_src"; then
			SPL_DIR=$(readlink -f $spl_src)
		else
			# Check /usr/src/spl-*/$LINUXRELEASE
			spl_src=$(ls -1d /usr/src/spl-*/$LINUXRELEASE \
			          2>/dev/null | tail -1)
			if test -e "$spl_src"; then
				SPL_DIR=$(readlink -f $spl_src)
			else
				# Check ../spl
				spl_src=$PWD/../spl
				if test -e "$spl_src"; then
					SPL_DIR=$(readlink -f $spl_src)
				else
					# Fatal spl required for zfs builds
					AC_MSG_ERROR([Could not locate spl.])
				fi
			fi
		fi

		;;
	*)
		SPL_DIR=$(readlink -f $with_spl)
		with_spl='yes'
		;;
esac

AC_ARG_WITH([spl-obj],
	AC_HELP_STRING([--with-spl-obj=path], [set path to spl objects]),
	[
		if test x$with_spl = xyes; then
			SPL_OBJ="$withval"
		fi
	],[
		if test x$with_spl = xyes; then
			SPL_OBJ=$SPL_DIR
		fi
	])

AC_MSG_CHECKING([spl source directory])
AC_MSG_RESULT([$SPL_DIR])
AC_SUBST(SPL_DIR)

AC_MSG_CHECKING([spl object directory])
AC_MSG_RESULT([$SPL_OBJ])
AC_SUBST(SPL_OBJ)

LB_SPL_SYMVERS
LB_SPL_RELEASE
])

# Optionally configure/make the zfs sources.  If the sources are
# determined to reside in-tree this feature will automatically be
# enabled.  If the sources are not in-tree it will be disabled.
# Use --enable-zfs-build or --disable-zfs-build if you need to
# override this behavior.
AC_DEFUN([LB_ZFS_BUILD],
[AC_ARG_ENABLE([zfs-build],
	AC_HELP_STRING([--enable-zfs-build], [enable zfs configure/make]),
	[], [
		ZFS_DIR_INTREE=$(readlink -f $PWD/zfs)
		if test x$ZFS_DIR = x$ZFS_DIR_INTREE; then
			enable_zfs_build='yes'
		else
			enable_zfs_build='no'
		fi
	])

AC_MSG_CHECKING([whether to build zfs])
if test x$enable_zfs_build = xyes; then
	AC_MSG_RESULT([$enable_zfs_build])

	LB_CHECK_FILE([$ZFS_DIR/module/zfs/dmu.c], [], [
		AC_MSG_ERROR([Complete zfs sources must exist when building.])])

	LB_CHECK_FILE([$ZFS_DIR/configure], [], [
		AC_MSG_ERROR([Complete zfs sources must exist when building.])])

	ZFS_SUBDIR="$ZFS_DIR"
	AC_SUBST(ZFS_SUBDIR)
	AC_CONFIG_SUBDIRS("zfs")

	ac_configure_args="$ac_configure_args --with-spl=$SPL_DIR"
	ac_configure_args="$ac_configure_args --with-spl-obj=$SPL_OBJ"
else
	enable_zfs_build='no'
	AC_MSG_RESULT([$enable_zfs_build])
fi
])

AC_DEFUN([LB_ZFS_SYMVERS],
[AC_MSG_CHECKING([zfs module symbols])
if test -r $ZFS_OBJ/Module.symvers; then
	ZFS_SYMBOLS=Module.symvers
elif test -r $ZFS_OBJ/Modules.symvers; then
	ZFS_SYMBOLS=Modules.symvers
elif test -r $ZFS_OBJ/module/Module.symvers; then
	ZFS_SYMBOLS=Module.symvers
elif test -r $ZFS_OBJ/module/Modules.symvers; then
	ZFS_SYMBOLS=Modules.symvers
else
	ZFS_SYMBOLS=$SYMVERFILE
fi

AC_MSG_RESULT([$ZFS_SYMBOLS])
AC_SUBST(ZFS_SYMBOLS)
])

AC_DEFUN([LB_ZFS_RELEASE],
[AC_MSG_CHECKING([zfs source release])
if test -r $ZFS_OBJ/zfs_config.h; then
	tmp_flags="$EXTRA_KCFLAGS"
	EXTRA_KCFLAGS="-I$ZFS_DIR $EXTRA_KCFLAGS"
	LB_LINUX_TRY_MAKE([
		#include <$ZFS_OBJ/zfs_config.h>
	],[
		char *ZFS_RELEASE;
		ZFS_RELEASE=ZFS_META_VERSION;
	],[
		$makerule LUSTRE_KERNEL_TEST=conftest.i
	],[
		test -s build/conftest.i
	],[
		eval $(grep "ZFS_RELEASE=" build/conftest.i)
	],[
		AC_MSG_RESULT([unknown])
		AC_MSG_ERROR([Could not preprocess test program.])
	])
	EXTRA_KCFLAGS="$tmp_flags"
	rm build/conftest.i
elif test -r $ZFS_DIR/META; then
	ZFS_RELEASE=$(awk '/Version/ { print [$]2 }' $ZFS_DIR/META)
else
	AC_MSG_RESULT([unknown])
	AC_MSG_ERROR([Could not locate zfs_config.h or META to check release.])
fi

if test x$ZFS_RELEASE = x; then
	AC_MSG_RESULT([unknown])
	AC_MSG_ERROR([Could not determine zfs release.])
fi

AC_MSG_RESULT([$ZFS_RELEASE])
AC_SUBST(ZFS_RELEASE)
])

#
# LB_PATH_ZFS
# --without-zfs   - Disable zfs support.
# --with-zfs=no
#
# --with-zfs      - Enable zfs support and attempt to autodetect the zfs
# --with-zfs=yes    headers in one of the following places in this order:
#                   * ./zfs
#                   * /usr/src/zfs-*/$LINUXRELEASE
#                   * ../zfs
#
# --with-zfs=path - Enable zfs support and use the zfs headers in the
#                   provided path.  No autodetection is performed.
#
# --with-zfs-obj  - When zfs support is enabled the object directory
#                   will be based on the --with-zfs directory.  If this
#                   is detected incorrectly it can be explicitly
#                   specified using this option.
#
# NOTE: As with all external packages zfs is expected to already be
# configured and built.  However, if the zfs tree is located in-tree
# (./zfs) then it will be configured and built recursively as part of
# the lustre build system.
#
AC_DEFUN([LB_PATH_ZFS],
[AC_ARG_WITH([zfs],
	AC_HELP_STRING([--with-zfs=path], [set path to zfs sources]),
	[],[
		if test x$linux25$enable_server = xyesyes; then
			with_zfs='yes'
		else
			with_zfs='no'
		fi
	])

case x$with_zfs in
	xno)
		ZFS_DIR=
		;;
	xyes)
		ZFS_DIR=

		# Check ./zfs
		zfs_src=$PWD/zfs
		if test -e "$zfs_src"; then
			ZFS_DIR=$(readlink -f $zfs_src)
		else
			# Check /usr/src/zfs-*/$LINUXRELEASE
			zfs_src=$(ls -1d /usr/src/zfs-*/$LINUXRELEASE \
				2>/dev/null|tail -1)
			if test -e "$zfs_src"; then
				ZFS_DIR=$(readlink -f $zfs_src)
			else
				# Check ../zfs
				zfs_src=$PWD/../zfs
				if test -e "$zfs_src"; then
					ZFS_DIR=$(readlink -f $zfs_src)
				else
					# Disable zfs failed to detect sources
					with_zfs='no'
				fi
			fi
		fi

		;;
	*)
		ZFS_DIR=$(readlink -f $with_zfs)
		with_zfs='yes'
		;;
esac

AC_MSG_CHECKING([whether to enable zfs])
AC_MSG_RESULT([$with_zfs])

AC_ARG_WITH([zfs-obj],
	AC_HELP_STRING([--with-zfs-obj=path], [set path to zfs objects]),
	[
		if test x$with_zfs = xyes; then
			ZFS_OBJ="$withval"
		fi
	],[
		if test x$with_zfs = xyes; then
			ZFS_OBJ=$ZFS_DIR
		fi
	])

if test x$with_zfs = xyes; then
	LB_ZFS_DEFINE_OPTIONS

	AC_MSG_CHECKING([zfs source directory])
	AC_MSG_RESULT([$ZFS_DIR])
	AC_SUBST(ZFS_DIR)

	AC_MSG_CHECKING([zfs object directory])
	AC_MSG_RESULT([$ZFS_OBJ])
	AC_SUBST(ZFS_OBJ)

	LB_ZFS_SYMVERS
	LB_ZFS_RELEASE

	LB_PATH_SPL

	LB_SPL_BUILD
	LB_ZFS_BUILD

fi

AM_CONDITIONAL(SPL_BUILD, test x$enable_spl_build = xyes)
AM_CONDITIONAL(ZFS_BUILD, test x$enable_zfs_build = xyes)
AM_CONDITIONAL(ZFS_ENABLED, test x$with_zfs = xyes)
])

#
# LB_ZFS_DEFINE_OPTIONS
#
# Enable config options related to zfs.
#
AC_DEFUN([LB_ZFS_DEFINE_OPTIONS],
[
	AC_DEFINE(HAVE_ZFS_OSD, 1, Enable zfs osd)
])

#
# LB_PATH_SNMP
#
# check for in-tree snmp support
#
AC_DEFUN([LB_PATH_SNMP],
[LB_CHECK_FILE([$srcdir/snmp/lustre-snmp.c],[SNMP_DIST_SUBDIR="snmp"])
AC_SUBST(SNMP_DIST_SUBDIR)
AC_SUBST(SNMP_SUBDIR)
])

#
# LB_CONFIG_MODULES
#
# Build kernel modules?
#
AC_DEFUN([LB_CONFIG_MODULES],
[AC_MSG_CHECKING([whether to build kernel modules])
AC_ARG_ENABLE([modules],
	AC_HELP_STRING([--disable-modules],
			[disable building of Lustre kernel modules]),
	[],[
		LC_TARGET_SUPPORTED([
			enable_modules='yes'
		],[
			enable_modules='no'
		])
	])
AC_MSG_RESULT([$enable_modules ($target_os)])

if test x$enable_modules = xyes ; then
	case $target_os in
		linux*)
			LB_PROG_LINUX
			LIBCFS_PROG_LINUX
			LN_PROG_LINUX
			LC_PROG_LINUX
			;;
		darwin*)
			LB_PROG_DARWIN
			LIBCFS_PROG_DARWIN
			;;
		*)
			# This is strange - Lustre supports a target we don't
			AC_MSG_ERROR([Modules are not supported on $target_os])
			;;
	esac
fi
])

#
# LB_CONFIG_UTILS
#
# Build utils?
#
AC_DEFUN([LB_CONFIG_UTILS],
[AC_MSG_CHECKING([whether to build utilities])
AC_ARG_ENABLE([utils],
	AC_HELP_STRING([--disable-utils],
			[disable building of Lustre utility programs]),
	[],[enable_utils='yes'])
AC_MSG_RESULT([$enable_utils])
if test x$enable_utils = xyes ; then 
	LB_CONFIG_INIT_SCRIPTS
fi
])

#
# LB_CONFIG_TESTS
#
# Build tests?
#
AC_DEFUN([LB_CONFIG_TESTS],
[AC_MSG_CHECKING([whether to build Lustre tests])
AC_ARG_ENABLE([tests],
	AC_HELP_STRING([--disable-tests],
			[disable building of Lustre tests]),
	[],
	[
		enable_tests='yes'
	])
AC_MSG_RESULT([$enable_tests])
])

#
# LB_CONFIG_DIST
#
# Just enough configure so that "make dist" is useful
#
# this simply re-adjusts some defaults, which of course can be overridden
# on the configure line after the --for-dist option
#
AC_DEFUN([LB_CONFIG_DIST],
[AC_MSG_CHECKING([whether to configure just enough for make dist])
AC_ARG_ENABLE([dist],
	AC_HELP_STRING([--enable-dist],
			[only configure enough for make dist]),
	[enable_dist='yes'],[enable_dist='no'])
AC_MSG_RESULT([$enable_dist])
if test x$enable_dist != xno; then
	enable_modules='no'
	enable_utils='no'
        enable_liblustre='no'
        enable_doc='no'
        enable_tests='no'
fi
])

#
# LB_CONFIG_DOCS
#
# Build docs?
#
AC_DEFUN([LB_CONFIG_DOCS],
[AC_MSG_CHECKING([whether to build docs])
AC_ARG_ENABLE(doc,
	AC_HELP_STRING([--disable-doc],
			[skip creation of pdf documentation]),
	[
		if test x$enable_doc = xyes ; then
		    ENABLE_DOC=1
		else
		    ENABLE_DOC=0
		fi
	],[
		ENABLE_DOC=0
		enable_doc='no'
	])
AC_MSG_RESULT([$enable_doc])
AC_SUBST(ENABLE_DOC)
])

#
# LB_CONFIG_INIT_SCRIPTS
#
# our init scripts only work on red hat linux
#
AC_DEFUN([LB_CONFIG_INIT_SCRIPTS],
[ENABLE_INIT_SCRIPTS=0
if test x$enable_utils = xyes ; then
        AC_MSG_CHECKING([whether to install init scripts])
        # our scripts only work on red hat systems
        if test -f /etc/init.d/functions -a -f /etc/sysconfig/network ; then
                ENABLE_INIT_SCRIPTS=1
                AC_MSG_RESULT([yes])
        else
                AC_MSG_RESULT([no])
        fi
fi
AC_SUBST(ENABLE_INIT_SCRIPTS)
])

#
# LB_CONFIG_HEADERS
#
# add -include config.h
#
AC_DEFUN([LB_CONFIG_HEADERS],
[AC_CONFIG_HEADERS([config.h])
CPPFLAGS="-include $PWD/config.h $CPPFLAGS"
EXTRA_KCFLAGS="-include $PWD/config.h $EXTRA_KCFLAGS"
AC_SUBST(EXTRA_KCFLAGS)
])

#
# LB_LIBCFS_DIR
#
# Define no libcfs by default.
#
AC_DEFUN([LB_LIBCFS_DIR],
[
case x$libcfs_is_module in
	xyes)
          LIBCFS_INCLUDE_DIR="libcfs/include"
          LIBCFS_SUBDIR="libcfs"
          ;;
        x*)
          LIBCFS_INCLUDE_DIR="lnet/include"
          LIBCFS_SUBDIR=""
          ;;
esac
AC_SUBST(LIBCFS_SUBDIR)
AC_SUBST(LIBCFS_INCLUDE_DIR)
])

#
# LB_INCLUDE_RULES
#
# defines for including the toplevel Rules
#
AC_DEFUN([LB_INCLUDE_RULES],
[INCLUDE_RULES="include $PWD/Rules"
AC_SUBST(INCLUDE_RULES)
])

#
# LB_PATH_DEFAULTS
#
# 'fixup' default paths
#
AC_DEFUN([LB_PATH_DEFAULTS],
[# directories for binaries
AC_PREFIX_DEFAULT([/usr])

sysconfdir='/etc'
AC_SUBST(sysconfdir)

# Directories for documentation and demos.
docdir='${datadir}/doc/$(PACKAGE)'
AC_SUBST(docdir)

LIBCFS_PATH_DEFAULTS
LN_PATH_DEFAULTS
LC_PATH_DEFAULTS

])

#
# LB_PROG_CC
#
# checks on the C compiler
#
AC_DEFUN([LB_PROG_CC],
[AC_PROG_RANLIB
AC_MSG_CHECKING([for buggy compiler])
CC_VERSION=`$CC -v 2>&1 | grep "^gcc version"`
bad_cc() {
	AC_MSG_RESULT([buggy compiler found!])
	echo
	echo "   '$CC_VERSION'"
	echo "  has been known to generate bad code, "
	echo "  please get an updated compiler."
	AC_MSG_ERROR([sorry])
}
case "$CC_VERSION" in
	"gcc version 2.95"*)
		bad_cc
		;;
	# ost_pack_niobuf putting 64bit NTOH temporaries on the stack
	# without "sub    $0xc,%esp" to protect the stack from being
	# stomped on by interrupts (bug 606)
	"gcc version 2.96 20000731 (Red Hat Linux 7.1 2.96-98)")
		bad_cc
		;;
	# mandrake's similar sub 0xc compiler bug
	# http://marc.theaimsgroup.com/?l=linux-kernel&m=104748366226348&w=2
	"gcc version 2.96 20000731 (Mandrake Linux 8.1 2.96-0.62mdk)")
		bad_cc
		;;
	*)
		AC_MSG_RESULT([no known problems])
		;;
esac

# ---------  unsigned long long sane? -------
AC_CHECK_SIZEOF(unsigned long long, 0)
echo "---> size SIZEOF $SIZEOF_unsigned_long_long"
echo "---> size SIZEOF $ac_cv_sizeof_unsigned_long_long"
if test $ac_cv_sizeof_unsigned_long_long != 8 ; then
        AC_MSG_ERROR([** we assume that sizeof(long long) == 8.  Tell phil@clusterfs.com])
fi

if test $target_cpu == "powerpc64"; then
	AC_MSG_WARN([set compiler with -m64])
	CFLAGS="$CFLAGS -m64"
	CC="$CC -m64"
fi

CPPFLAGS="-I$PWD/$LIBCFS_INCLUDE_DIR -I$PWD/lnet/include -I$PWD/lustre/include $CPPFLAGS"

LLCPPFLAGS="-D__arch_lib__ -D_LARGEFILE64_SOURCE=1"
AC_SUBST(LLCPPFLAGS)

# Add _GNU_SOURCE for strnlen on linux
LLCFLAGS="-g -Wall -fPIC -D_GNU_SOURCE"
AC_SUBST(LLCFLAGS)

# everyone builds against lnet and lustre
EXTRA_KCFLAGS="$EXTRA_KCFLAGS -g -I$PWD/$LIBCFS_INCLUDE_DIR -I$PWD/lnet/include -I$PWD/lustre/include"
AC_SUBST(EXTRA_KCFLAGS)
])

#
# LB_CONTITIONALS
#
# AM_CONDITIONAL instances for everything
# (so that portals/lustre can disable some if needed)
AC_DEFUN([LB_CONDITIONALS],
[AM_CONDITIONAL(MODULES, test x$enable_modules = xyes)
AM_CONDITIONAL(UTILS, test x$enable_utils = xyes)
AM_CONDITIONAL(TESTS, test x$enable_tests = xyes)
AM_CONDITIONAL(DOC, test x$ENABLE_DOC = x1)
AM_CONDITIONAL(INIT_SCRIPTS, test x$ENABLE_INIT_SCRIPTS = "x1")
AM_CONDITIONAL(LINUX, test x$lb_target_os = "xlinux")
AM_CONDITIONAL(DARWIN, test x$lb_target_os = "xdarwin")
AM_CONDITIONAL(CRAY_XT3, test x$enable_cray_xt3 = "xyes")
AM_CONDITIONAL(SUNOS, test x$lb_target_os = "xSunOS")
AM_CONDITIONAL(USES_DPKG, test x$uses_dpkg = "xyes")

# this lets lustre cancel libsysio, per-branch or if liblustre is
# disabled
if test "x$LIBSYSIO_SUBDIR" = xlibsysio ; then
	if test "x$with_sysio" != xyes ; then
		SYSIO=""
		LIBSYSIO_SUBDIR=""
	fi
fi
AC_SUBST(LIBSYSIO_SUBDIR)
AC_SUBST(SYSIO)

LB_LINUX_CONDITIONALS
LB_DARWIN_CONDITIONALS

LIBCFS_CONDITIONALS
LN_CONDITIONALS
LC_CONDITIONALS
])

#
# LB_CONFIG_FILES
#
# build-specific config files
#
AC_DEFUN([LB_CONFIG_FILES],
[
AC_CONFIG_FILES(
[Makefile
autoMakefile
]
[Rules:build/Rules.in]
AC_PACKAGE_TARNAME[.spec]
)
])

#
# LB_CONFIGURE
#
# main configure steps
#
AC_DEFUN([LB_CONFIGURE],
[LB_CANONICAL_SYSTEM

LB_CONFIG_DIST

LB_DOWNSTREAM_RELEASE
LB_USES_DPKG
LB_BUILDID

LB_LIBCFS_DIR

LB_INCLUDE_RULES

LB_CONFIG_CRAY_XT3
LB_CONFIG_BGL
LB_PATH_DEFAULTS

LB_PROG_CC

LC_OSD_ADDON

LB_CONFIG_DOCS
LB_CONFIG_UTILS
LB_CONFIG_TESTS
LC_CONFIG_CLIENT_SERVER

# two macros for cmd3
m4_ifdef([LC_CONFIG_SPLIT], [LC_CONFIG_SPLIT])
LN_CONFIG_CDEBUG
LC_QUOTA

LB_CONFIG_MODULES
LN_CONFIG_USERSPACE

LB_PATH_LIBSYSIO
LB_PATH_SNMP
LB_PATH_LDISKFS
LB_PATH_ZFS
LB_PATH_LUSTREIOKIT

LB_DEFINE_E2FSPROGS_NAMES

LC_CONFIG_LIBLUSTRE
LIBCFS_CONFIGURE
LN_CONFIGURE

LC_CONFIGURE

if test "$SNMP_DIST_SUBDIR" ; then
	LS_CONFIGURE
fi


LB_CONDITIONALS
LB_CONFIG_HEADERS

LIBCFS_CONFIG_FILES
LB_CONFIG_FILES
LN_CONFIG_FILES
LC_CONFIG_FILES
if test "$SNMP_DIST_SUBDIR" ; then
	LS_CONFIG_FILES
fi

AC_SUBST(ac_configure_args)

MOSTLYCLEANFILES='.*.cmd .*.flags *.o *.ko *.mod.c .depend .*.1.* Modules.symvers Module.symvers'
AC_SUBST(MOSTLYCLEANFILES)

AC_OUTPUT

cat <<_ACEOF

CC:            $CC
LD:            $LD
CPPFLAGS:      $CPPFLAGS
LLCPPFLAGS:    $LLCPPFLAGS
CFLAGS:        $CFLAGS
EXTRA_KCFLAGS: $EXTRA_KCFLAGS
LLCFLAGS:      $LLCFLAGS

Type 'make' to build Lustre.
_ACEOF
])
