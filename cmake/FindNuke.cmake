include(FindPackageHandleStandardArgs)

set(_NUKE_ROOT_HINTS)
if(NUKE_ROOT)
  list(APPEND _NUKE_ROOT_HINTS "${NUKE_ROOT}")

  if(APPLE)
    if(EXISTS "${NUKE_ROOT}/Contents/MacOS")
      list(APPEND _NUKE_ROOT_HINTS "${NUKE_ROOT}/Contents/MacOS")
    endif()

    get_filename_component(_NUKE_ROOT_NAME "${NUKE_ROOT}" NAME)
    if(EXISTS "${NUKE_ROOT}/${_NUKE_ROOT_NAME}.app/Contents/MacOS")
      list(APPEND _NUKE_ROOT_HINTS "${NUKE_ROOT}/${_NUKE_ROOT_NAME}.app/Contents/MacOS")
    endif()
  endif()
endif()

if(WIN32)
  list(APPEND _NUKE_ROOT_HINTS
    "$ENV{ProgramFiles}/Nuke15.0v1"
    "$ENV{ProgramFiles}/Nuke15.0v2"
    "$ENV{ProgramFiles}/Nuke15.1v1"
    "$ENV{ProgramFiles}/Nuke15.2v1"
  )
endif()

find_path(NUKE_INCLUDE_DIR
  NAMES DDImage/Writer.h
  HINTS ${_NUKE_ROOT_HINTS}
  PATH_SUFFIXES include include/DDImage Contents/MacOS/include Contents/MacOS/include/DDImage
)

find_library(NUKE_DDIMAGE_LIBRARY
  NAMES DDImage
  HINTS ${_NUKE_ROOT_HINTS}
  PATH_SUFFIXES lib libs bin Contents/MacOS
)

find_package_handle_standard_args(Nuke
  REQUIRED_VARS
    NUKE_INCLUDE_DIR
    NUKE_DDIMAGE_LIBRARY
)

if(Nuke_FOUND AND NOT TARGET Nuke::DDImage)
  add_library(Nuke::DDImage UNKNOWN IMPORTED)
  set_target_properties(Nuke::DDImage PROPERTIES
    IMPORTED_LOCATION "${NUKE_DDIMAGE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${NUKE_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(
  NUKE_INCLUDE_DIR
  NUKE_DDIMAGE_LIBRARY
)
