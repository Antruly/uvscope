cmake_minimum_required(VERSION 3.20)

project(uvscope
  VERSION 0.5.2
  DESCRIPTION "A libuvcpp-powered multi-protocol web debugging workbench and gateway"
  HOMEPAGE_URL "https://github.com/Antruly/uvscope"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(UVSCOPE_BUILD_TESTS "Build uvscope tests" ON)
option(UVSCOPE_PACKAGE_RUNTIME "Install the libuvcpp runtime beside uvscope" ON)

# ---------------------------------------------------------------------------
# libuvcpp SDK
#
# uvscope deliberately has a single network/application stack: libuvcpp.
# Point UVCPP_ROOT at a release SDK (the self-contained zip from
# https://github.com/Antruly/libuvcpp/releases). No package manager or
# additional networking library is required.
# ---------------------------------------------------------------------------
set(UVCPP_ROOT "" CACHE PATH "Path to an extracted libuvcpp 1.8.2 SDK")
set(_uvcpp_hints "")
if(UVCPP_ROOT)
  list(APPEND _uvcpp_hints "${UVCPP_ROOT}")
endif()
if(DEFINED ENV{UVCPP_ROOT} AND NOT "$ENV{UVCPP_ROOT}" STREQUAL "")
  list(APPEND _uvcpp_hints "$ENV{UVCPP_ROOT}")
endif()

find_path(UVCPP_INCLUDE_DIR
  NAMES uvcpp/uvcpp_config.h
  HINTS ${_uvcpp_hints}
  PATH_SUFFIXES include
  DOC "libuvcpp public headers")
find_library(UVCPP_LIBRARY
  NAMES uvcpp libuvcpp
  HINTS ${_uvcpp_hints}
  PATH_SUFFIXES lib lib64
  DOC "libuvcpp library")
if(NOT UVCPP_INCLUDE_DIR OR NOT UVCPP_LIBRARY)
  message(FATAL_ERROR
    "libuvcpp 1.8.2 was not found.\n"
    "Download a release SDK and configure with -DUVCPP_ROOT=/path/to/libuvcpp-1.8.2-<platform>,\n"
    "or set the UVCPP_ROOT environment variable. See docs/BUILDING.md.")
endif()
get_filename_component(UVCPP_LIBRARY_DIR "${UVCPP_LIBRARY}" DIRECTORY)
message(STATUS "libuvcpp headers: ${UVCPP_INCLUDE_DIR}")
message(STATUS "libuvcpp library: ${UVCPP_LIBRARY}")

# uvscope requires the 1.8.2 SDK: TLS peer chains, SOCKS5 session statistics,
# AF_UNSPEC resolution and the blocking-I/O deadline are all part of it. Fail
# here instead of silently building a runtime that is missing those paths.
set(_uvcpp_version_header "${UVCPP_INCLUDE_DIR}/uvcpp/uvcpp_version.h")
if(NOT EXISTS "${_uvcpp_version_header}")
  message(FATAL_ERROR
    "libuvcpp version header missing from ${UVCPP_INCLUDE_DIR}; use a 1.8.2 release SDK.")
endif()
file(STRINGS "${_uvcpp_version_header}" _uvcpp_version_major
  REGEX "^#define UVCPP_VERSION_MAJOR ([0-9]+)$")
file(STRINGS "${_uvcpp_version_header}" _uvcpp_version_minor
  REGEX "^#define UVCPP_VERSION_MINOR ([0-9]+)$")
file(STRINGS "${_uvcpp_version_header}" _uvcpp_version_patch
  REGEX "^#define UVCPP_VERSION_PATCH ([0-9]+)$")
string(REGEX REPLACE ".* ([0-9]+)$" "\\1" UVCPP_VERSION_MAJOR "${_uvcpp_version_major}")
string(REGEX REPLACE ".* ([0-9]+)$" "\\1" UVCPP_VERSION_MINOR "${_uvcpp_version_minor}")
string(REGEX REPLACE ".* ([0-9]+)$" "\\1" UVCPP_VERSION_PATCH "${_uvcpp_version_patch}")
if(NOT UVCPP_VERSION_MAJOR MATCHES "^[0-9]+$"
   OR NOT UVCPP_VERSION_MINOR MATCHES "^[0-9]+$"
   OR NOT UVCPP_VERSION_PATCH MATCHES "^[0-9]+$")
  message(FATAL_ERROR "Could not parse the libuvcpp version from ${_uvcpp_version_header}.")
endif()
set(UVCPP_DETECTED_VERSION
  "${UVCPP_VERSION_MAJOR}.${UVCPP_VERSION_MINOR}.${UVCPP_VERSION_PATCH}")
message(STATUS "libuvcpp version: ${UVCPP_DETECTED_VERSION}")
if(UVCPP_DETECTED_VERSION VERSION_LESS "1.8.2")
  message(FATAL_ERROR
    "libuvcpp ${UVCPP_DETECTED_VERSION} is too old; uvscope requires 1.8.2 or newer.\n"
    "Rebuild the SDK with ./scripts/build-libuvcpp.sh or point UVCPP_ROOT at a 1.8.2 release.")
endif()

# libuvcpp.dll is required next to the executable on Windows. The archive's
# import library and DLL live in predictable places; locate them for install.
if(WIN32)
  find_file(UVCPP_RUNTIME_DLL
    NAMES uvcpp.dll libuvcpp.dll
    HINTS ${_uvcpp_hints}
    PATH_SUFFIXES bin lib
    DOC "libuvcpp runtime DLL")
  find_file(UVCPP_LLHTTP_RUNTIME_DLL
    NAMES libllhttp.dll
    HINTS ${_uvcpp_hints}
    PATH_SUFFIXES bin lib
    DOC "llhttp runtime DLL required by libuvcpp")
endif()

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

# libuv headers are needed by libuvcpp's inline/public headers. Link libuvcpp,
# not a second libuv: the daemon must use exactly the event loop the framework
# was built against.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(UVCPP_LIBUV QUIET libuv)
endif()
if(NOT UVCPP_LIBUV_INCLUDE_DIRS)
  find_path(UVCPP_LIBUV_INCLUDE_DIR uv.h
    HINTS ${CMAKE_PREFIX_PATH}/include ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES}
          /opt/homebrew/include /usr/local/include /usr/include
    DOC "libuv public headers")
  if(UVCPP_LIBUV_INCLUDE_DIR)
    set(UVCPP_LIBUV_INCLUDE_DIRS "${UVCPP_LIBUV_INCLUDE_DIR}")
  endif()
endif()
if(NOT UVCPP_LIBUV_INCLUDE_DIRS)
  message(FATAL_ERROR "libuv headers (uv.h) were not found; install libuv development files")
endif()

# The transport manager uses libuv timers directly. Link the same libuv that
# libuvcpp was built against; it is also the event loop shared by the daemon.
find_library(UVSCOPE_LIBUV_LIBRARY
  NAMES uv libuv
  HINTS ${CMAKE_PREFIX_PATH}/lib /opt/homebrew/lib /usr/local/lib
  DOC "libuv runtime library")
if(NOT UVSCOPE_LIBUV_LIBRARY)
  message(FATAL_ERROR "libuv library was not found; install libuv development files")
endif()

# ---------------------------------------------------------------------------
# Version / build information
# ---------------------------------------------------------------------------
set(UVSCOPE_GITHUB_OWNER "Antruly" CACHE STRING "GitHub owner")
set(UVSCOPE_GITHUB_REPO "uvscope" CACHE STRING "GitHub repository")
configure_file(src/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
set(UVSCOPE_SOURCES
  src/main.cpp
  src/application.cpp
  src/auth.cpp
  src/certificate.cpp
  src/certificate_manager.cpp
  src/command_seed.cpp
  src/crypto.cpp
  src/download_manager.cpp
  src/http_probe.cpp
  src/net_probe.cpp
  src/preview_store.cpp
  src/quic_manager.cpp
  src/proxy_manager.cpp
  src/socks5_manager.cpp
  src/socks5_proxy.cpp
  src/store.cpp
  src/terminal_manager.cpp
  src/tls_probe.cpp
  src/transport_manager.cpp
  src/util.cpp
  src/ws_manager.cpp)

add_executable(uvscope ${UVSCOPE_SOURCES})

if(WIN32)
  # Embed the same mark used by the browser favicon into uvscope.exe so the
  # installer, shortcuts and the executable share one icon.
  enable_language(RC)
  set(UVSCOPE_ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons/uvscope.ico")
  configure_file(
    packaging/windows/uvscope.rc.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/uvscope.rc"
    @ONLY)
  target_sources(uvscope PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated/uvscope.rc")
endif()
target_include_directories(uvscope PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
  "${UVCPP_INCLUDE_DIR}"
  ${UVCPP_LIBUV_INCLUDE_DIRS})
target_link_libraries(uvscope PRIVATE
  "${UVCPP_LIBRARY}"
  "${UVSCOPE_LIBUV_LIBRARY}"
  OpenSSL::SSL
  OpenSSL::Crypto
  Threads::Threads
  ${CMAKE_DL_LIBS})
target_compile_definitions(uvscope PRIVATE
  UVSCOPE_VERSION="${PROJECT_VERSION}"
  UVSCOPE_GITHUB_OWNER="${UVSCOPE_GITHUB_OWNER}"
  UVSCOPE_GITHUB_REPO="${UVSCOPE_GITHUB_REPO}")

if(MSVC)
  target_compile_options(uvscope PRIVATE /W4 /permissive-)
else()
  target_compile_options(uvscope PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
  target_link_libraries(uvscope PRIVATE ws2_32 iphlpapi psapi userenv shell32)
  set_target_properties(uvscope PROPERTIES
    INSTALL_RPATH ""
    BUILD_WITH_INSTALL_RPATH FALSE)
else()
  if(APPLE)
    set(_uvscope_install_rpath "@executable_path/../${CMAKE_INSTALL_LIBDIR}/uvscope")
  else()
    set(_uvscope_install_rpath "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/uvscope")
  endif()
  set_target_properties(uvscope PROPERTIES
    BUILD_RPATH "${UVCPP_LIBRARY_DIR}"
    INSTALL_RPATH "${_uvscope_install_rpath}"
    BUILD_WITH_INSTALL_RPATH FALSE)
endif()

# ---------------------------------------------------------------------------
# Install layout
#
# Linux:  /usr/bin/uvscope
#         /usr/lib/uvscope/libuvcpp.so
#         /usr/share/uvscope/web
# Windows: bin/uvscope.exe, bin/libuvcpp.dll, web/
# ---------------------------------------------------------------------------
install(TARGETS uvscope RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

set(UVSCOPE_WEB_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/web/dist")
if(NOT EXISTS "${UVSCOPE_WEB_SOURCE}/index.html")
  message(WARNING
    "web/dist/index.html does not exist. Build the frontend first "
    "(npm --prefix web ci && npm --prefix web run build).")
endif()
install(DIRECTORY "${UVSCOPE_WEB_SOURCE}/"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/uvscope/web"
  OPTIONAL)

if(UVSCOPE_PACKAGE_RUNTIME)
  if(WIN32 AND UVCPP_RUNTIME_DLL)
    install(FILES "${UVCPP_RUNTIME_DLL}" DESTINATION "${CMAKE_INSTALL_BINDIR}")
    if(UVCPP_LLHTTP_RUNTIME_DLL)
      install(FILES "${UVCPP_LLHTTP_RUNTIME_DLL}" DESTINATION "${CMAKE_INSTALL_BINDIR}")
    endif()
  elseif(UNIX)
    install(FILES "${UVCPP_LIBRARY}" DESTINATION "${CMAKE_INSTALL_LIBDIR}/uvscope")
    file(GLOB _uvcpp_runtime_libs
      "${UVCPP_LIBRARY_DIR}/libllhttp.so*"
      "${UVCPP_LIBRARY_DIR}/libllhttp*.dylib"
      "${UVCPP_LIBRARY_DIR}/libuv.so*"
      "${UVCPP_LIBRARY_DIR}/libuv*.dylib"
      "${UVCPP_LIBRARY_DIR}/libz.so*"
      "${UVCPP_LIBRARY_DIR}/libz*.dylib")
    if(_uvcpp_runtime_libs)
      install(FILES ${_uvcpp_runtime_libs} DESTINATION "${CMAKE_INSTALL_LIBDIR}/uvscope")
    endif()
  endif()
endif()

# Locate OpenSSL runtime DLLs on MinGW so the release archive is runnable.
if(WIN32 AND UVSCOPE_PACKAGE_RUNTIME)
  get_filename_component(_ssl_lib_dir "${OPENSSL_SSL_LIBRARY}" DIRECTORY)
  set(_ssl_candidates
    "${_ssl_lib_dir}"
    "${_ssl_lib_dir}/../bin"
    "${OPENSSL_ROOT_DIR}/bin"
    "${CMAKE_PREFIX_PATH}/bin")
  foreach(_dir ${_ssl_candidates})
    if(EXISTS "${_dir}")
      file(GLOB _openssl_dlls
        "${_dir}/libssl-*.dll"
        "${_dir}/libcrypto-*.dll")
      if(_openssl_dlls)
        install(FILES ${_openssl_dlls} DESTINATION "${CMAKE_INSTALL_BINDIR}")
      endif()
    endif()
  endforeach()
endif()

install(FILES
  README.md
  LICENSE
  CHANGELOG.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/uvscope"
  OPTIONAL)

if(UNIX AND NOT APPLE)
  install(FILES packaging/linux/uvscope.desktop
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
endif()

# Desktop icons. Linux desktops find them through the hicolor theme
# (`Icon=uvscope` in the desktop entry); macOS keeps the .icns in the share
# tree for future .app bundling.
if(UNIX)
  install(FILES packaging/icons/uvscope.svg
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
    RENAME uvscope.svg OPTIONAL)
  foreach(_size 16 24 32 48 64 128 256 512)
    install(FILES "packaging/icons/uvscope-${_size}.png"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${_size}x${_size}/apps"
      RENAME uvscope.png OPTIONAL)
  endforeach()
endif()
if(APPLE)
  install(FILES packaging/icons/uvscope.icns
    DESTINATION "${CMAKE_INSTALL_DATADIR}/uvscope" OPTIONAL)
endif()

# ---------------------------------------------------------------------------
# CPack (Linux deb/rpm/tgz). Windows artifacts are assembled by the workflow,
# where the MinGW runtime and the Inno Setup installer are available.
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "uvscope")
set(CPACK_PACKAGE_VENDOR "Antruly")
set(CPACK_PACKAGE_CONTACT "Antruly <antruly@users.noreply.github.com>")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Multi-protocol web debugging workbench and gateway")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Antruly/uvscope")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_EXECUTABLES "uvscope" "uvscope")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Antruly <antruly@users.noreply.github.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6, libstdc++6, libssl3, libuv1, zlib1g, libsqlite3-0")
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_REQUIRES "openssl-libs, libuv, zlib, sqlite-libs")
set(CPACK_STRIP_FILES ON)
set(CPACK_GENERATOR "TGZ")
include(CPack)

# ---------------------------------------------------------------------------
# Tests (fast, no network)
# ---------------------------------------------------------------------------
if(UVSCOPE_BUILD_TESTS)
  enable_testing()
  string(REPLACE "." "\\." UVSCOPE_VERSION_REGEX "${PROJECT_VERSION}")
  add_test(NAME uvscope_version
    COMMAND uvscope --version)
  set_tests_properties(uvscope_version PROPERTIES
    PASS_REGULAR_EXPRESSION "uvscope ${UVSCOPE_VERSION_REGEX}")

  add_test(NAME uvscope_selfcheck
    COMMAND uvscope --self-check)
  set_tests_properties(uvscope_selfcheck PROPERTIES TIMEOUT 30)
endif()
