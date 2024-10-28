dnl #
dnl # Determine if tools required for the agent are installed
dnl #
AC_DEFUN([ZFS_AC_CONFIG_ALWAYS_RUST], [
	AC_MSG_CHECKING([for rust build tools])
	AS_IF([cargo &>/dev/null],
		[ac_rust_tools="yes"], [ac_rust_tools="no"])

	AC_MSG_RESULT([$ac_rust_tools])
	AM_CONDITIONAL([BUILD_RUST], [test "x$ac_rust_tools" = "xyes"])
])
