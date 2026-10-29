# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# Unit tests: SOAP envelopes and faults, inventory and session handling, CBT paging, the streamOptimized VMDK
# encoder, grain framing, disk attach and import spec building. All of them run against a scripted fake HTTP
# transport, so no vCenter or ESXi host is needed.

find_package(GTest 1.10 CONFIG QUIET)
if(NOT TARGET GTest::gtest_main)
    if(NOT VSPHERE_SOAP_FETCH_DEPS)
        message(FATAL_ERROR "vsphere-soap-lib: GoogleTest not found and VSPHERE_SOAP_FETCH_DEPS is OFF")
    endif()
    message(STATUS "vsphere-soap-lib: GoogleTest not found -- fetching GoogleTest")
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW ON
    )
    FetchContent_MakeAvailable(googletest)
endif()
include(GoogleTest)

add_executable(vsphere_soap_tests
    test_soap_client.cpp
    test_datastore_path.cpp
    test_cbt_extents.cpp
    test_vmdk_encoder.cpp
    test_vmdk_decoder.cpp
    test_export_stream.cpp
    test_grain_assembler.cpp
    test_disk_attach.cpp
    test_vim_list_objects.cpp
    test_vsphere_network_mapping.cpp
    test_vsphere_disk_provisioning.cpp
    test_log.cpp
    test_placement.cpp
)

# White-box: the tests drive the library's internal classes through their private headers.
target_include_directories(vsphere_soap_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(vsphere_soap_tests PRIVATE vsphere-soap GTest::gtest_main)
if(TARGET vsphere-soap-disk)
    target_sources(vsphere_soap_tests PRIVATE test_disk_adapter.cpp)
    target_link_libraries(vsphere_soap_tests PRIVATE vsphere-soap-disk)
endif()
gtest_discover_tests(vsphere_soap_tests)

# The encoder's compressed output read back by a third-party reader: qemu-img must accept the VMDK and
# convert it back to exactly the raw image it was built from. Disabled when qemu-img is not on PATH.
add_executable(vmdk_encoder_writer vmdk_encoder_writer.cpp)
target_include_directories(vmdk_encoder_writer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(vmdk_encoder_writer PRIVATE vsphere-soap)

find_program(QEMU_IMG_EXECUTABLE qemu-img)
add_test(
    NAME vmdk_encoder_qemu_img_interop
    COMMAND "${CMAKE_COMMAND}"
        "-DWRITER=$<TARGET_FILE:vmdk_encoder_writer>"
        "-DQEMU_IMG=${QEMU_IMG_EXECUTABLE}"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/vmdk_encoder_qemu_img_interop_work"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/vmdk_encoder_qemu_img_interop.cmake"
)
if(NOT QEMU_IMG_EXECUTABLE)
    set_tests_properties(vmdk_encoder_qemu_img_interop PROPERTIES DISABLED TRUE)
endif()
