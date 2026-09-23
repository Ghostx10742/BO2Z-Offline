# To find libtomcrypt no matter what the installation look like, start with
# looking for the CMake specific configuration, and failing that, try the
# pkg-config package instead.  The resulting target is different in each
# case, but is recorded in the variable ${LIBTOMCRYPT}, so please use that
# for all targets that depend on libtomcrypt.

find_package(libtomcrypt QUIET)
if (libtomcrypt_FOUND)
  set(LIBTOMCRYPT libtomcrypt)
else()
  find_package(PkgConfig)
  pkg_check_modules(libtomcrypt REQUIRED IMPORTED_TARGET libtomcrypt)
  set(LIBTOMCRYPT PkgConfig::libtomcrypt)
endif()
