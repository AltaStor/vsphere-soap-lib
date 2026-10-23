# SPDX-License-Identifier: Apache-2.0
# Copyright © 2026 Norvitex OÜ
#
# vsphere-disk: export a VM disk to a raw image or import a raw image as a new VM, through the public API only.

add_executable(vsphere-disk vsphere-disk.cpp)
target_link_libraries(vsphere-disk PRIVATE VSphereSoap::vsphere-soap)
if(MINGW)
    target_link_options(vsphere-disk PRIVATE -static)
endif()
