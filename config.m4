PHP_ARG_ENABLE([swmongo],
  [whether to enable swmongo support],
  [AS_HELP_STRING([--enable-swmongo], [Enable swmongo (coroutine MongoDB driver for Swoole)])],
  [no])

if test "$PHP_SWMONGO" != "no"; then
  PHP_CHECK_LIBRARY(z, compress2, [
    PHP_ADD_LIBRARY(z, 1, SWMONGO_SHARED_LIBADD)
  ], [
    AC_MSG_ERROR([zlib is required by swmongo])
  ])
  PHP_SUBST(SWMONGO_SHARED_LIBADD)

  dnl Files that perform I/O (and can therefore yield to another coroutine) must not
  dnl use floating point registers: Swoole's arm64 context switch does not preserve
  dnl the callee-saved FP registers d8-d15. -mgeneral-regs-only turns any FP use in
  dnl those files into a compile error.
  SWM_IO_CFLAGS=""
  case $host_cpu in
    aarch64*|arm64*)
      dnl zend_types.h unconditionally includes arm_neon.h (unused by the headers); skip it.
      SWM_IO_CFLAGS="-mgeneral-regs-only -D__ARM_NEON_H=1"
      ;;
  esac

  PHP_NEW_EXTENSION(swmongo, swm_util.c, $ext_shared,, -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1 -Wall -Wno-unused-parameter)
  PHP_ADD_SOURCES_X(PHP_EXT_DIR(swmongo), swmongo.c swm_bson.c swm_client.c swm_driver.c, -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1 -Wall -Wno-unused-parameter $SWM_IO_CFLAGS, shared_objects_swmongo, yes)
  PHP_ADD_EXTENSION_DEP(swmongo, json)
  PHP_ADD_EXTENSION_DEP(swmongo, date)
  PHP_ADD_EXTENSION_DEP(swmongo, spl)
  PHP_ADD_EXTENSION_DEP(swmongo, hash)
fi
