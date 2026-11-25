// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ (AltaStor.com)
//
// Unit tests for vSphere disk unit selection around non-disk devices (CD/DVD drives)

#include <gtest/gtest.h>

#include "DiskAttach.hpp"

using VSphereSoap::Detail::chooseControllerAndUnit;
using VSphereSoap::Detail::ControllerSelector;
using VSphereSoap::Vim::VimControllerInfo;
using VSphereSoap::Vim::VimVmInfo;

namespace {

VimControllerInfo controllerWith(int key, std::string type, int busNumber, std::vector<int> usedUnits = {})
{
    VimControllerInfo c;
    c.key = key;
    c.deviceType = std::move(type);
    c.busNumber = busNumber;
    c.usedUnitNumbers = std::move(usedUnits);
    return c;
}

} // namespace

// V2V-68: a CD/DVD drive at unit 0 (counted in usedUnitNumbers by describeVm) pushes the disk to the next unit.
TEST(ChooseControllerUnitAroundCdromTest, SkipsUnitTakenByCdrom)
{
    VimVmInfo target; // the STR's VM: LSI Logic SAS, SATA with a CD/DVD drive at 0:0, IDE with one at 0:0
    target.controllers = {controllerWith(1000, "VirtualLsiLogicSASController", 0),
                          controllerWith(15000, "VirtualAHCIController", 0, {0}),
                          controllerWith(200, "VirtualIDEController", 0, {0})};

    ControllerSelector sata;
    sata.controllerType = "sata";
    sata.busNumber = 0;
    auto result = chooseControllerAndUnit(target, sata);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().controllerKey, 15000);
    EXPECT_EQ(result.value().unitNumber, 1);

    auto automatic = chooseControllerAndUnit(target, ControllerSelector{});
    ASSERT_TRUE(automatic);
    // V2V-94: on a VM without disks SCSI wins over SATA and IDE (it used to be IDE, the lowest key).
    EXPECT_EQ(automatic.value().controllerKey, 1000);
    EXPECT_EQ(automatic.value().unitNumber, 0);
}

// The automatic choice passes over a controller whose every unit is taken, and fails only when all are.
TEST(ChooseControllerUnitAroundCdromTest, AutomaticSkipsFullControllers)
{
    VimVmInfo target;
    target.controllers = {controllerWith(200, "VirtualIDEController", 0, {0, 1}),
                          controllerWith(201, "VirtualIDEController", 1, {0, 1}),
                          controllerWith(1000, "VirtualLsiLogicSASController", 0)};
    auto result = chooseControllerAndUnit(target, ControllerSelector{});
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().controllerKey, 1000);
    EXPECT_EQ(result.value().unitNumber, 0);

    target.controllers.pop_back();
    auto none = chooseControllerAndUnit(target, ControllerSelector{});
    ASSERT_FALSE(none);
    EXPECT_NE(none.error().message().find("--target-controller-create"), std::string::npos);
}
