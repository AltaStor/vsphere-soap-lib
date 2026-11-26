# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ (AltaStor.com)
#
# vsphere-soap-lib: export and import of virtual disk images from/to vSphere over the vSphere Web Services
# (SOAP) API and plain HTTPS, with no VMware SDK.
#
# Configured two ways:
#
#   on its own       cmake -S . -B build
#                    The library, its unit tests, and an installable package (find_package(VSphereSoap)).
#
#   from a tool      add_subdirectory("${VSPHERE_SOAP_LIB_DIR}" vsphere_soap_lib_build)
#                    target_link_libraries(my-tool PRIVATE VSphereSoap::vsphere-soap)
#                    The library only, built with the tool's own toolchain and settings.
#
# Targets:
#
#   VSphereSoap::vsphere-soap        the core: SOAP layer, export and import, Error/Result and the log hook
#   VSphereSoap::vsphere-soap-disk   optional IDisk interface with adapters over the core disk classes
#
# Parameters:
#
#   VSPHERE_SOAP_BUILD_TESTS     ON | OFF   the unit tests (default: ON standalone, OFF from a tool)
#   VSPHERE_SOAP_BUILD_EXAMPLES  ON | OFF   the vsphere-disk example tool (default: ON standalone)
#   VSPHERE_SOAP_BUILD_DISK      ON | OFF   the vsphere-soap-disk target (default: ON standalone)
#   VSPHERE_SOAP_FETCH_DEPS      ON | OFF   fetch Boost, pugixml and GoogleTest when the system lacks them (ON)
#   VSPHERE_SOAP_INSTALL         ON | OFF   install rules and the CMake package (default: ON standalone)
cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(VSPHERE_SOAP_STANDALONE TRUE)

    project(vsphere-soap-lib VERSION 1.0.0 LANGUAGES CXX)

    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    endif()
else()
    set(VSPHERE_SOAP_STANDALONE FALSE)
    set(PROJECT_VERSION 1.0.0)
endif()

option(VSPHERE_SOAP_BUILD_TESTS "Build vsphere-soap-lib's unit tests" ${VSPHERE_SOAP_STANDALONE})
option(VSPHERE_SOAP_BUILD_EXAMPLES "Build the vsphere-disk example tool" ${VSPHERE_SOAP_STANDALONE})
option(VSPHERE_SOAP_BUILD_DISK "Build the optional IDisk interface and its adapters" ${VSPHERE_SOAP_STANDALONE})
option(VSPHERE_SOAP_FETCH_DEPS "Fetch Boost, pugixml and GoogleTest when they are not installed" ON)
option(VSPHERE_SOAP_INSTALL "Install the library, its headers and the VSphereSoap CMake package" ${VSPHERE_SOAP_STANDALONE})

# Keeps a conda or similar environment on PATH from supplying its own, ABI-incompatible Boost or OpenSSL.
set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH OFF)

include(FetchContent)
include(GNUInstallDirs)

# --- Dependencies ----------------------------------------------------------------------------------------------
#
# OpenSSL, zlib and threads are expected from the system on every target.
# Boost (Asio and Beast only, both header-only here) and pugixml are fetched when missing, so a fresh machine
# or a cross build needs nothing beyond a compiler, OpenSSL and zlib.

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

set(_vsphere_soap_fetched "")

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()
find_package(Boost 1.75 CONFIG QUIET)
if(Boost_FOUND)
    set(_vsphere_soap_boost_targets Boost::headers)
elseif(TARGET Boost::asio AND TARGET Boost::beast)
    # A parent project already fetched Boost from source.
    set(_vsphere_soap_boost_targets Boost::asio Boost::beast)
elseif(VSPHERE_SOAP_FETCH_DEPS)
    message(STATUS "vsphere-soap-lib: Boost not found -- fetching Boost.Asio and Boost.Beast")
    set(BOOST_INCLUDE_LIBRARIES asio beast)
    set(BOOST_ENABLE_CMAKE ON)
    FetchContent_Declare(
        Boost
        URL https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-cmake.tar.xz
        URL_HASH SHA256=2c5ec5edcdff47ff55e27ed9560b0a0b94b07bd07ed9928b476150e16b0efc57
        DOWNLOAD_EXTRACT_TIMESTAMP ON
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(Boost)
    set(_vsphere_soap_boost_targets Boost::asio Boost::beast)
    list(APPEND _vsphere_soap_fetched Boost)
else()
    message(FATAL_ERROR "vsphere-soap-lib: Boost 1.75+ not found and VSPHERE_SOAP_FETCH_DEPS is OFF")
endif()

find_package(pugixml 1.11 QUIET)
if(NOT TARGET pugixml::pugixml)
    if(NOT VSPHERE_SOAP_FETCH_DEPS)
        message(FATAL_ERROR "vsphere-soap-lib: pugixml 1.11+ not found and VSPHERE_SOAP_FETCH_DEPS is OFF")
    endif()
    message(STATUS "vsphere-soap-lib: pugixml not found -- fetching pugixml")
    set(PUGIXML_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        pugixml
        GIT_REPOSITORY https://github.com/zeux/pugixml.git
        GIT_TAG v1.14
        GIT_SHALLOW ON
    )
    FetchContent_MakeAvailable(pugixml)
    if(NOT TARGET pugixml::pugixml)
        add_library(pugixml::pugixml ALIAS pugixml)
    endif()
    list(APPEND _vsphere_soap_fetched pugixml)
endif()

# --- The library -----------------------------------------------------------------------------------------------

add_library(vsphere-soap STATIC
    src/Error.cpp
    src/Log.cpp
    src/HttpsRestClient.cpp
    src/SoapClient.cpp
    src/VimTypes.cpp
    src/VimSession.cpp
    src/VimInventory.cpp
    src/SnapshotScope.cpp
    src/CbtQuery.cpp
    src/DatastorePath.cpp
    src/TaskWait.cpp
    src/DiskAttach.cpp
    src/StreamOptimizedVmdkEncoder.cpp
    src/StreamOptimizedVmdkDecoder.cpp
    src/ExportStreamReader.cpp
    src/HttpNfcLeaseSession.cpp
    src/GrainAssembler.cpp
    src/VmwareSoapDisk.cpp
    src/VSphereVmNetworkInfo.cpp
    src/VSphereDatastoreBackend.cpp
    src/VSphereImportSession.cpp
    src/VSphereDiskImport.cpp
    src/VSphereFolderPutBackend.cpp
    src/VSphereFolderPutImport.cpp
    src/VSpherePlacement.cpp
    src/VmLookup.cpp
    src/VimConnection.cpp
    src/VSphereClient.cpp
)
add_library(VSphereSoap::vsphere-soap ALIAS vsphere-soap)

target_include_directories(vsphere-soap
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# Boost is header-only for this library and is needed to compile its sources, never by a consumer:
# BUILD_INTERFACE keeps it out of the installed package's link interface.
target_link_libraries(vsphere-soap
    PRIVATE
        "$<BUILD_INTERFACE:${_vsphere_soap_boost_targets}>"
    PUBLIC
        pugixml::pugixml
        OpenSSL::SSL
        OpenSSL::Crypto
        ZLIB::ZLIB
        Threads::Threads
)

if(WIN32)
    # Winsock for Boost.Asio, crypt32 for OpenSSL's certificate store access.
    target_link_libraries(vsphere-soap PUBLIC ws2_32 mswsock crypt32)
    target_compile_definitions(vsphere-soap PRIVATE _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

target_compile_features(vsphere-soap PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(vsphere-soap PRIVATE /utf-8 /W3 /bigobj)
else()
    target_compile_options(vsphere-soap PRIVATE -Wall -Wextra)
    if(MINGW)
        # Beast's templates overflow the default COFF section count.
        target_compile_options(vsphere-soap PRIVATE -Wa,-mbig-obj)
    endif()
endif()

set_target_properties(vsphere-soap PROPERTIES
    OUTPUT_NAME vsphere-soap
    POSITION_INDEPENDENT_CODE ON
)

# --- The optional IDisk interface ------------------------------------------------------------------------------
#
# Kept out of the core so a tool with a disk interface of its own links only the core and adapts the disk
# classes itself.

if(VSPHERE_SOAP_BUILD_DISK)
    add_library(vsphere-soap-disk STATIC src/disk/IDisk.cpp)
    add_library(VSphereSoap::vsphere-soap-disk ALIAS vsphere-soap-disk)
    target_link_libraries(vsphere-soap-disk PUBLIC vsphere-soap)
    if(MSVC)
        target_compile_options(vsphere-soap-disk PRIVATE /utf-8 /W3)
    else()
        target_compile_options(vsphere-soap-disk PRIVATE -Wall -Wextra)
    endif()
    set_target_properties(vsphere-soap-disk PROPERTIES POSITION_INDEPENDENT_CODE ON)
    list(APPEND _vsphere_soap_install_targets vsphere-soap-disk)
endif()

message(STATUS "vsphere-soap-lib ${PROJECT_VERSION}: ${CMAKE_SYSTEM_NAME} target")

# --- The unit tests --------------------------------------------------------------------------------------------

if(VSPHERE_SOAP_BUILD_TESTS)
    if(VSPHERE_SOAP_STANDALONE)
        enable_testing()
    endif()
    add_subdirectory(tests)
endif()

if(VSPHERE_SOAP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# --- Install and the CMake package -----------------------------------------------------------------------------
#
# The installed package only refers to dependencies the system provides: a fetched pugixml has no installed
# package of its own for find_dependency() to find, so installing is refused rather than producing a package
# that fails in its consumer.

if(VSPHERE_SOAP_INSTALL)
    if(pugixml IN_LIST _vsphere_soap_fetched)
        message(STATUS "vsphere-soap-lib: pugixml was fetched, so VSPHERE_SOAP_INSTALL is ignored. "
                        "Install libpugixml-dev (or point pugixml_DIR at it) to install this library.")
    else()
        include(CMakePackageConfigHelpers)

        install(TARGETS vsphere-soap ${_vsphere_soap_install_targets} EXPORT VSphereSoapTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
        if(VSPHERE_SOAP_BUILD_DISK)
            install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
        else()
            install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} PATTERN disk EXCLUDE)
        endif()
        # A path of its own, since from a tool the default DOCDIR is named after the tool's project
        install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/vsphere-soap-lib)
        install(EXPORT VSphereSoapTargets
            NAMESPACE VSphereSoap::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VSphereSoap
        )
        configure_package_config_file(cmake/VSphereSoapConfig.cmake.in
            "${CMAKE_CURRENT_BINARY_DIR}/VSphereSoapConfig.cmake"
            INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VSphereSoap
        )
        write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/VSphereSoapConfigVersion.cmake"
            VERSION ${PROJECT_VERSION}
            COMPATIBILITY SameMajorVersion
        )
        install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/VSphereSoapConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/VSphereSoapConfigVersion.cmake"
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/VSphereSoap
        )
    endif()
endif()
