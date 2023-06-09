find_package(PkgConfig REQUIRED)
pkg_check_modules(PC_liburing REQUIRED liburing)

find_path(liburing_INCLUDE_DIR
  NAMES liburing.h
  PATHS ${PC_liburing_INCLUDE_DIRS}
)
find_library(liburing_LIBRARY
  NAMES uring
  PATHS ${PC_liburing_LIBRARY_DIRS}
)

set(liburing_VERSION ${PC_liburing_VERSION})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(liburing
  FOUND_VAR liburing_FOUND
  REQUIRED_VARS
    liburing_LIBRARY
    liburing_INCLUDE_DIR
  VERSION_VAR liburing_VERSION
)

if(liburing_FOUND AND NOT TARGET liburing::liburing)
  add_library(liburing::liburing UNKNOWN IMPORTED)
  set_target_properties(liburing::liburing PROPERTIES
    IMPORTED_LOCATION "${liburing_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${liburing_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(
  liburing_INCLUDE_DIR
  liburing_LIBRARY
)
