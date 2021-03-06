AC_INIT([tpm2-abrmd],
        [m4_esyscmd_s([cat ./VERSION])],
        [https://github.com/01org/tpm2-abrmd/issues],
        [],
        [https://github.com/01org/tpm2-abrmd])
AC_CONFIG_MACRO_DIR([m4])
AC_PROG_CC
AC_PROG_LN_S
LT_INIT()
AM_INIT_AUTOMAKE([foreign subdir-objects])
# enable "silent-rules" option by default
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])
AC_CONFIG_FILES([Makefile])

# propagate configure arguments to distcheck
AC_SUBST([DISTCHECK_CONFIGURE_FLAGS],[$ac_configure_args])

AX_PTHREAD([], [AC_MSG_ERROR([requires pthread])])
AC_ARG_ENABLE([unit],
              [AS_HELP_STRING([--enable-unit],
                   [build cmocka unit tests (default is no)])],
              [enable_unit=$enableval],
              [enable_unit=no])
AS_IF([test "x$enable_unit" != xno],
      [PKG_CHECK_MODULES([CMOCKA],
                         [cmocka >= 1.0],
                         [AC_DEFINE([HAVE_CMOCKA],
                                    [1],
                                    [cmocka is available])])])
AM_CONDITIONAL([UNIT], [test "x$enable_unit" != xno])

# -dl or -dld
AC_SEARCH_LIBS([dlopen], [dl dld], [], [
  AC_MSG_ERROR([unable to find the dlopen() function])
])
PKG_CHECK_MODULES([DBUS], [dbus-1])
PKG_CHECK_MODULES([GIO], [gio-unix-2.0])
PKG_CHECK_MODULES([GLIB], [glib-2.0])
PKG_CHECK_MODULES([GOBJECT], [gobject-2.0])
PKG_CHECK_MODULES([SAPI],[sapi >= 2.0.0])
AC_ARG_VAR([GDBUS_CODEGEN],[The gdbus-codegen executable.])
AC_PATH_PROG([GDBUS_CODEGEN], [`$PKG_CONFIG --variable=gdbus_codegen gio-2.0`])
if test -z "$GDBUS_CODEGEN"; then
    AC_MSG_ERROR([*** gdbus-codegen is required to build tpm2-abrmd])
fi

AX_CODE_COVERAGE
# disable helgrind and drd, they hate GAsyncQueue
AX_VALGRIND_DFLT([sgcheck], [off])
AX_VALGRIND_DFLT([helgrind], [off])
AX_VALGRIND_DFLT([drd], [off])
AX_VALGRIND_CHECK

# allow
AC_ARG_ENABLE([dlclose],
  [AS_HELP_STRING([--disable-dlclose],
    [Some versions of libc cause a sigsegv on exit, this disables the dlclose and works around that bug])],
  []
  [AC_DEFINE([DISABLE_DLCLOSE], [1])]
)

# function from the gnu.org docs
AC_DEFUN([MY_ARG_WITH],
         [AC_ARG_WITH(m4_translit([[$1]], [_], [-]),
                      [AS_HELP_STRING([--with-m4_translit([$1], [_], [-])],
                                      [use $1 (default is $2)])],
                      [use_[]$1=$withval],
                      [use_[]$1=$2])
         ])

#
# systemd
#
AC_ARG_WITH([systemdsystemunitdir],
            AS_HELP_STRING([--with-systemdsystemunitdir=DIR],
                           [Directory for systemd service files]),
            [],
            [with_systemdsystemunitdir=${libdir}/systemd/system])
AS_IF([test "x$with_systemdsystemunitdir" != xno],
      [AC_SUBST([systemdsystemunitdir],
      [$with_systemdsystemunitdir])])
AM_CONDITIONAL(HAVE_SYSTEMD, [test -n "$with_systemdsystemunitdir" -a "x$with_systemdsystemunitdir" != xno ])

# systemd preset directory
AC_ARG_WITH([systemdpresetdir],
            AS_HELP_STRING([--with-systemdpresetdir=DIR],
                           [Directory for systemd preset files]),
            [],
            [with_systemdpresetdir=${libdir}/systemd/system-preset])
AC_SUBST([systemdpresetdir], [$with_systemdpresetdir])

# systemd preset default (enable / disable)
AC_ARG_WITH([systemdpresetdisable],
             AS_HELP_STRING([--with-systemdpresetdisable],
                            [Configure systemd preset to 'disable', default is 'enable']),
            [AC_SUBST([SYSTEMD_PRESET_DEFAULT],[disable])],
            [AC_SUBST([SYSTEMD_PRESET_DEFAULT],[enable])])

#
# dbus
#
AC_ARG_WITH([dbuspolicydir],
            [AS_HELP_STRING([--with-dbuspolicydir=DIR],[D-Bus policy directory])],
            [],
            [with_dbuspolicydir=${sysconfdir}/dbus-1/system.d])
AX_NORMALIZE_PATH([with_dbuspolicydir])
AC_SUBST([dbuspolicydir], [$with_dbuspolicydir])
#
# udev
#
AC_ARG_WITH([udevrulesdir],
            [AS_HELP_STRING([--with-udevrulesdir=DIR],[udev rules directory])],
            [],
            [with_udevrulesdir=${libdir}/udev/rules.d])
AX_NORMALIZE_PATH([with_udevrulesdir])
AC_SUBST([udevrulesdir], [$with_udevrulesdir])
AC_ARG_WITH([udevrulesprefix],
            [AS_HELP_STRING([--with-udevrulesprefix=XY],[prefix for udev rules file])],
            [AC_SUBST([udevrulesprefix],[$with_udevrulesprefix])])
AM_CONDITIONAL(WITH_UDEVRULESPREFIX, [test -n "$with_udevrulesprefix"])
#
# simulator binary
#
AC_MSG_CHECKING([for simulator binary: $with_simulatorbin])
AC_ARG_WITH([simulatorbin],
            [AS_HELP_STRING([--with-simulatorbin=tpm_server],[simulator executable])],
            [AS_IF([test \( -f "$with_simulatorbin" \) -a \( -x "$with_simulatorbin" \)],
                   [AC_MSG_RESULT([yes])
                    AC_SUBST([SIMULATOR_BIN],[$with_simulatorbin])
                    AX_NORMALIZE_PATH([with_simulatorbin])
                    with_simulatorbin_set=yes],
                   [AC_MSG_RESULT([no])
                    AC_MSG_ERROR([Simulator binary provided does not exist or not executable. Check path: $with_simulatorbin])])],
            [AC_MSG_RESULT([no])
             with_simulatorbin_set=no])
AM_CONDITIONAL([SIMULATOR_BIN],[test "x$with_simulatorbin_set" = "xyes"])
#
# Real TPM hardware
#
AC_ARG_ENABLE([test-hwtpm],
              [AS_HELP_STRING([--enable-test-hwtpm],
                  [enable the integration test on a real tpm hardware (default is no)])],
              [enable_hwtpm=$enableval],
              [enable_hwtpm=no])
AS_IF([test \( "x$with_simulatorbin_set" = "xyes" \) -a \( "x$enable_hwtpm" = "xyes" \) ],
      AC_MSG_ERROR([Simulator binary and real tpm hardware cannot be both enabled.]))
AM_CONDITIONAL([HWTPM], [test "x$enable_hwtpm" != xno])

AS_IF([test \( "x$with_simulatorbin_set" = "xno" \) -a \( "x$enable_hwtpm" = "xno" \) ],
      AC_MSG_WARN([No simulator binary or tpm hardware provided. Integration tests disabled.]))

# preprocessor / compiler / linker flags
#   these macros are defined in m4/flags.m4
AX_ADD_COMPILER_FLAG([-Wall])
AX_ADD_COMPILER_FLAG([-Werror])
AX_ADD_COMPILER_FLAG([-std=gnu99])
AX_ADD_COMPILER_FLAG([-Wformat])
AX_ADD_COMPILER_FLAG([-Wformat-security])
AX_ADD_COMPILER_FLAG([-Wno-missing-braces])
AX_ADD_COMPILER_FLAG([-fdata-sections])
AX_ADD_COMPILER_FLAG([-ffunction-sections])
AX_ADD_COMPILER_FLAG([-fstack-protector-all])
AX_ADD_COMPILER_FLAG([-fpic])
AX_ADD_COMPILER_FLAG([-fPIC])
AX_ADD_PREPROC_FLAG([-D_GNU_SOURCE])
AX_ADD_PREPROC_FLAG([-U_FORTIFY_SOURCE])
AX_ADD_PREPROC_FLAG([-D_FORTIFY_SOURCE=2])
AX_ADD_LINK_FLAG([-Wl,--gc-sections])
AX_ADD_LINK_FLAG([-Wl,--no-undefined])
AX_ADD_LINK_FLAG([-Wl,-z,noexecstack])
AX_ADD_LINK_FLAG([-Wl,-z,now])
AX_ADD_LINK_FLAG([-Wl,-z,relro])

AC_OUTPUT
