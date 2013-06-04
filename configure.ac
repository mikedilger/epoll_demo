dnl Autoconf and Automake setup
AC_PREREQ(2.59)
AC_INIT([xframedb], [GIT_UNKNOWN], [mike@mikedilger.com], [xframedb])
AC_COPYRIGHT([Unspecified])
AC_REVISION([$Revision: GIT_UNKNOWN $])
AC_CONFIG_SRCDIR([configure.ac])
AC_CONFIG_MACRO_DIR([m4])
AC_LANG([C])
AC_LANG_C
AM_INIT_AUTOMAKE([foreign])

AC_CONFIG_HEADERS([config.h])

dnl Checks for necessary programs
AC_PROG_INSTALL
AC_PROG_MAKE_SET
AC_PROG_CC

dnl Check headers
AC_HEADER_STDC
AC_HEADER_DIRENT
AC_HEADER_SYS_WAIT
AC_CHECK_HEADERS([arpa/inet.h fcntl.h netdb.h netinet/in.h stdint.h stdlib.h string.h sys/socket.h unistd.h])

AC_CONFIG_FILES([Makefile src/Makefile])

AC_OUTPUT
