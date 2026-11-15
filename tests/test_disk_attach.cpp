// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Unit tests for vSphere disk controller/unit selection

#include <gtest/gtest.h>

#include "DiskAttach.hpp"

// Unit tests for chooseControllerAndUnit(), the pure controller/unit resolution algorithm behind
// the disk-attach flow.
// No network needed; this function takes a hand-built VimVmInfo fixture and a ControllerSelector, and
// its only job is picking (or asking to create) a controller/unit, so it's tested directly rather than
// through FakeHttpTransport like the SOAP-calling classes in this test binary.

using VSphereSoap::Detail::ControllerSelector;
using VSphereSoap::Vim::VimControllerInfo;
using VSphereSoap::Vim::VimDiskInfo;
using VSphereSoap::Vim::VimVmInfo;
using VSphereSoap::Detail::chooseControllerAndUnit;

namespace {

VimControllerInfo makeController(int key, std::string type, int busNumber, std::vector<int> usedUnits = {})
{
    VimControllerInfo c;
    c.key = key;
    c.deviceType = std::move(type);
    c.busNumber = busNumber;
    c.usedUnitNumbers = std::move(usedUnits);
    return c;
}

VimDiskInfo makeDiskOnController(int controllerKey, int unitNumber)
{
    VimDiskInfo d;
    d.controllerKey = controllerKey;
    d.unitNumber = unitNumber;
    return d;
}

} // namespace

TEST(ChooseControllerAndUnitTest, AutomaticPicksPrimaryControllerByDiskCountAndLowestFreeUnit)
{
    VimVmInfo target;
    target.controllers = {makeController(1000, "ParaVirtualSCSIController", 0, {0, 1}),
                            makeController(1001, "VirtualAHCIController", 0, {0})};
    target.disks = {makeDiskOnController(1000, 0), makeDiskOnController(1000, 1), makeDiskOnController(1001, 0)};

    auto result = chooseControllerAndUnit(target, ControllerSelector{});
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().controllerKey, 1000); // 2 disks vs. AHCI's 1
    EXPECT_EQ(result.value().unitNumber, 2);        // 0 and 1 used, 7 reserved but irrelevant here
    EXPECT_FALSE(result.value().controllerToCreate);
}

TEST(ChooseControllerAndUnitTest, AutomaticSkipsReservedScsiUnitSeven)
{
    VimVmInfo target;
    // Units 0..6 all used on this SCSI controller: the next free one must be 8, not 7 (reserved for
    // the controller device itself).
    target.controllers = {makeController(1000, "VirtualLsiLogicController", 0, {0, 1, 2, 3, 4, 5, 6})};
    for (int u = 0; u <= 6; ++u) {
        target.disks.push_back(makeDiskOnController(1000, u));
    }

    auto result = chooseControllerAndUnit(target, ControllerSelector{});
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().unitNumber, 8);
}

TEST(ChooseControllerAndUnitTest, AutomaticTiesBrokenByLowestControllerKey)
{
    VimVmInfo target;
    // Neither controller has any disk yet: a tie on disk count (0 vs 0).
    target.controllers = {makeController(2000, "ParaVirtualSCSIController", 1), makeController(1000, "ParaVirtualSCSIController", 0)};

    auto result = chooseControllerAndUnit(target, ControllerSelector{});
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().controllerKey, 1000);
    EXPECT_EQ(result.value().unitNumber, 0);
}

TEST(ChooseControllerAndUnitTest, AutomaticFailsWhenNoControllerExistsAndCreationNotRequested)
{
    VimVmInfo target; // no controllers at all
    auto result = chooseControllerAndUnit(target, ControllerSelector{});
    EXPECT_FALSE(result);
}

TEST(ChooseControllerAndUnitTest, AutomaticRequestsParavirtualControllerCreationWhenNoneExistAndAllowed)
{
    VimVmInfo target;
    ControllerSelector selector;
    selector.createIfMissing = true;

    auto result = chooseControllerAndUnit(target, selector);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result.value().controllerToCreate);
    EXPECT_EQ(result.value().controllerToCreate->first, "ParaVirtualSCSIController");
    EXPECT_EQ(result.value().controllerToCreate->second, 0);
}

TEST(ChooseControllerAndUnitTest, ExplicitBusNumberSelectsThatControllerRegardlessOfDiskCount)
{
    VimVmInfo target;
    target.controllers = {makeController(1000, "ParaVirtualSCSIController", 0, {0}),
                            makeController(1001, "VirtualAHCIController", 0, {})};
    target.disks = {makeDiskOnController(1000, 0)};

    ControllerSelector selector;
    selector.busNumber = 0;
    selector.controllerType = "sata";

    auto result = chooseControllerAndUnit(target, selector);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().controllerKey, 1001);
    EXPECT_EQ(result.value().unitNumber, 0);
}

TEST(ChooseControllerAndUnitTest, ExplicitBusNumberWithNoMatchFailsWithoutCreateIfMissing)
{
    VimVmInfo target;
    target.controllers = {makeController(1000, "ParaVirtualSCSIController", 0)};

    ControllerSelector selector;
    selector.busNumber = 5;

    auto result = chooseControllerAndUnit(target, selector);
    EXPECT_FALSE(result);
}

TEST(ChooseControllerAndUnitTest, ExplicitBusNumberWithNoMatchRequestsCreationWhenAllowed)
{
    VimVmInfo target;
    ControllerSelector selector;
    selector.busNumber = 2;
    selector.controllerType = "nvme";
    selector.createIfMissing = true;

    auto result = chooseControllerAndUnit(target, selector);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result.value().controllerToCreate);
    EXPECT_EQ(result.value().controllerToCreate->first, "VirtualNVMEController");
    EXPECT_EQ(result.value().controllerToCreate->second, 2);
}

TEST(ChooseControllerAndUnitTest, TypeOnlySelectsLowestBusControllerOfThatType)
{
    VimVmInfo target;
    target.controllers = {makeController(1000, "ParaVirtualSCSIController", 1),
                            makeController(1001, "ParaVirtualSCSIController", 0),
                            makeController(1002, "VirtualAHCIController", 0)};

    ControllerSelector selector;
    selector.controllerType = "paravirtual";

    auto result = chooseControllerAndUnit(target, selector);
    ASSERT_TRUE(result);
    EXPECT_EQ(result.value().controllerKey, 1001); // bus 0, not bus 1
}

TEST(ChooseControllerAndUnitTest, RejectsUnknownControllerTypeName)
{
    VimVmInfo target;
    ControllerSelector selector;
    selector.controllerType = "not-a-real-type";

    auto result = chooseControllerAndUnit(target, selector);
    EXPECT_FALSE(result);
}

TEST(ChooseControllerAndUnitTest, FailsWhenTheOnlyMatchingControllerHasNoFreeUnits)
{
    VimVmInfo target;
    // IDE controllers only have units 0 and 1.
    target.controllers = {makeController(1000, "VirtualIDEController", 0, {0, 1})};

    ControllerSelector selector;
    selector.controllerType = "ide";

    auto result = chooseControllerAndUnit(target, selector);
    EXPECT_FALSE(result);
}

// The advice in each "no such controller" error names the CLI option that sets createIfMissing,
// --target-controller-create, not the struct field.
TEST(ChooseControllerAndUnitTest, MissingControllerErrorsNameTheCliCreateOption)
{
    constexpr const char* kHint = "(pass --target-controller-create to add one)";

    VimVmInfo target; // the STR's VM: LSI Logic SAS on SCSI 0 and a SATA controller, no paravirtual
    target.controllers = {makeController(1000, "VirtualLsiLogicSASController", 0),
                          makeController(1001, "VirtualAHCIController", 0)};

    ControllerSelector onBus;
    onBus.controllerType = "paravirtual";
    onBus.busNumber = 1;
    auto busResult = chooseControllerAndUnit(target, onBus);
    ASSERT_FALSE(busResult);
    EXPECT_EQ(busResult.error().message(),
              std::string("target VM has no controller at bus 1 of type paravirtual ") + kHint);

    ControllerSelector ofType;
    ofType.controllerType = "paravirtual";
    auto typeResult = chooseControllerAndUnit(target, ofType);
    ASSERT_FALSE(typeResult);
    EXPECT_EQ(typeResult.error().message(), std::string("target VM has no paravirtual controller ") + kHint);

    auto noneResult = chooseControllerAndUnit(VimVmInfo{}, ControllerSelector{});
    ASSERT_FALSE(noneResult);
    EXPECT_EQ(noneResult.error().message(), std::string("target VM has no disk controller at all ") + kHint);

    for (const std::string& message :
         {busResult.error().message(), typeResult.error().message(), noneResult.error().message()}) {
        EXPECT_EQ(message.find("createIfMissing"), std::string::npos) << message;
    }
}
