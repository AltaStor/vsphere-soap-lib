// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Unit tests for datastore path parsing

#include <gtest/gtest.h>

#include "DatastorePath.hpp"
#include "vsphere-soap/vim/VimInventory.hpp"

namespace VSphereSoap::Detail {
namespace {

using namespace VSphereSoap::Vim;

TEST(ParseDatastorePathTest, ParsesBracketedDatastorePath)
{
    const auto result = parseDatastorePath("[datastore1] myvm/myvm.vmdk");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->first, "datastore1");
    EXPECT_EQ(result->second, "myvm/myvm.vmdk");
}

TEST(ParseDatastorePathTest, RejectsShapesThatArentBracketedPaths)
{
    EXPECT_FALSE(parseDatastorePath("myvm/myvm.vmdk").has_value());
    EXPECT_FALSE(parseDatastorePath("").has_value());
    EXPECT_FALSE(parseDatastorePath("[datastore1]").has_value());
    EXPECT_FALSE(parseDatastorePath("[] myvm.vmdk").has_value());
}

TEST(DeriveFlatNameTest, DerivesFlatNameFromDescriptorName)
{
    EXPECT_EQ(deriveFlatName("myvm/myvm.vmdk"), "myvm/myvm-flat.vmdk");
    EXPECT_EQ(deriveFlatName("myvm.vmdk"), "myvm-flat.vmdk");
}

TEST(DeriveFlatNameTest, LeavesAlreadyFlatOrDeltaNamesUnchanged)
{
    EXPECT_EQ(deriveFlatName("myvm/myvm-flat.vmdk"), "myvm/myvm-flat.vmdk");
    EXPECT_EQ(deriveFlatName("myvm/myvm-000001-delta.vmdk"), "myvm/myvm-000001-delta.vmdk");
}

TEST(DeriveFlatNameTest, LeavesNonVmdkNamesUnchanged)
{
    EXPECT_EQ(deriveFlatName("myvm/myvm.raw"), "myvm/myvm.raw");
}

TEST(SiblingPathTest, ReplacesLastPathSegment)
{
    EXPECT_EQ(siblingPath("myvm/myvm.vmdk", "myvm-flat.vmdk"), "myvm/myvm-flat.vmdk");
    EXPECT_EQ(siblingPath("a/b/myvm.vmdk", "myvm-flat.vmdk"), "a/b/myvm-flat.vmdk");
}

TEST(SiblingPathTest, HandlesNoDirectoryComponent)
{
    EXPECT_EQ(siblingPath("myvm.vmdk", "myvm-flat.vmdk"), "myvm-flat.vmdk");
}

TEST(UrlEncodeTest, EncodesReservedCharactersButPreservesSlash)
{
    EXPECT_EQ(urlEncode("my vm/dir name/file.vmdk"), "my%20vm/dir%20name/file.vmdk");
}

TEST(UrlEncodeTest, LeavesUnreservedCharactersUnchanged)
{
    EXPECT_EQ(urlEncode("abc-._~123"), "abc-._~123");
}

TEST(ParseFlatExtentFileNameTest, FindsExtentLineInDescriptor)
{
    const std::string descriptor = "# Disk DescriptorFile\n"
                                    "version=1\n"
                                    "CID=fffffffe\n"
                                    "parentCID=ffffffff\n"
                                    "createType=\"vmfs\"\n"
                                    "\n"
                                    "# Extent description\n"
                                    "RW 41943040 VMFS \"myvm-flat.vmdk\"\n"
                                    "\n"
                                    "# The Disk Data Base\n"
                                    "#DDB\n";
    const auto extent = parseFlatExtentFileName(descriptor);
    ASSERT_TRUE(extent.has_value());
    EXPECT_EQ(*extent, "myvm-flat.vmdk");
}

TEST(ParseFlatExtentFileNameTest, SkipsCommentAndHeaderLines)
{
    const std::string descriptor = "# comment mentioning \"quotes\" that isn't an extent line\n"
                                    "version=1\n"
                                    "RDONLY 2048 VMFS \"myvm-flat.vmdk\"\n";
    const auto extent = parseFlatExtentFileName(descriptor);
    ASSERT_TRUE(extent.has_value());
    EXPECT_EQ(*extent, "myvm-flat.vmdk");
}

TEST(ParseFlatExtentFileNameTest, ReturnsNulloptWhenNoExtentLineFound)
{
    EXPECT_FALSE(parseFlatExtentFileName("# just a comment\nversion=1\n").has_value());
}

} // namespace
} // namespace VSphereSoap::Detail
