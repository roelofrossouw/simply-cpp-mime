cmake_minimum_required(VERSION 3.22)
set(SC_MODULE mime)
project(simply-cpp-${SC_MODULE} HOMEPAGE_URL "https://github.com/roelofrossouw/simply-cpp-mime" LANGUAGES CXX)
include(cmake/sc_bootstrap.cmake)

option(SC_MIME_BUILD_TESTS "Build simply-cpp-mime tests" ${PROJECT_IS_TOP_LEVEL})

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

find_or_install_package(sc-core simply-cpp-dev simply-cpp 1.20.0)
# GMime 3 (LGPL-2.1-or-later) parses the messages; it's built on GLib (gobject, glib), whose
# functions the code calls directly too, so a static library's users link all three.
config_or_install_package(gmime-3.0 libgmime-3.0-dev gmime)
foreach (library gmime-3.0 gobject-2.0 glib-2.0)
    string(MAKE_C_IDENTIFIER "SC_MIME_${library}_LIBRARY" variable)
    if (${variable} AND NOT EXISTS "${${variable}}")
        message(STATUS "${library}: the cached library location is gone, looking again")
        unset(${variable} CACHE)
    endif ()
    find_library(${variable} NAMES ${library} HINTS ${gmime-3.0_LIBRARY_DIRS} REQUIRED)
    list(APPEND SC_MIME_GMIME_LIBRARIES ${${variable}})
endforeach ()

set(SOURCE_OBJECTS)
set(SOURCE_LIBRARIES)
set(SOURCE_LINK_LIBRARIES)

add_sc_object(sc_mime
        LINK_LIBRARIES sc::sc-core PkgConfig::gmime-3.0
        PUBLIC_LINK_LIBRARIES sc::sc-core ${SC_MIME_GMIME_LIBRARIES})

add_sc_libraries(DESCRIPTION "simply-cpp MIME mail messages: headers, addresses, body text and attachments (GMime)")
target_compile_features(sc-mime PUBLIC cxx_std_20)
target_compile_features(sc-mime-shared PUBLIC cxx_std_20)

install_sc_module(PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
install(FILES cmake/SimplyCppFunctions.cmake cmake/sc_bootstrap.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sc-mime
        COMPONENT development)

if (SC_MIME_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests sc-mime-test)
endif ()

add_sc_example(sc-mime-demo)

package_sc_module(DEV_DEPENDS "simply-cpp-dev (>= 1.20.0~)" libgmime-3.0-dev)
