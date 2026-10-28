// SPDX-License-Identifier: Apache-2.0
// Copyright © 2026 Norvitex OÜ
//
// Log sink routing and level filtering

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "SoapLog.hpp"
#include "vsphere-soap/Log.hpp"

namespace {

using VSphereSoap::Log::Level;

struct Captured {
    Level level;
    std::string channel;
    std::string message;
};

// Installs a capturing sink for one test and puts the defaults back afterwards.
class LogTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        m_savedLevel = VSphereSoap::Log::level();
        VSphereSoap::Log::setSink([this](Level level, std::string_view channel, std::string_view message) {
            m_captured.push_back({level, std::string(channel), std::string(message)});
        });
    }

    void TearDown() override
    {
        VSphereSoap::Log::setSink({});
        VSphereSoap::Log::setLevel(m_savedLevel);
    }

    std::vector<Captured> m_captured;

private:
    Level m_savedLevel = Level::Warn;
};

TEST_F(LogTest, FormatsArgumentsAndNamesTheChannel)
{
    VSphereSoap::Log::setLevel(Level::Debug);

    VSphereSoap::Detail::logger()->debug("lease {} at {}%", "session-7", 42);
    VSphereSoap::Detail::httpLogger()->warn("connect to {}:{} failed", "esx01", 443);

    ASSERT_EQ(m_captured.size(), 2u);
    EXPECT_EQ(m_captured[0].level, Level::Debug);
    EXPECT_EQ(m_captured[0].channel, "soap");
    EXPECT_EQ(m_captured[0].message, "lease session-7 at 42%");
    EXPECT_EQ(m_captured[1].level, Level::Warn);
    EXPECT_EQ(m_captured[1].channel, "http");
    EXPECT_EQ(m_captured[1].message, "connect to esx01:443 failed");
}

TEST_F(LogTest, DropsMessagesBelowTheLevel)
{
    VSphereSoap::Log::setLevel(Level::Warn);

    VSphereSoap::Detail::logger()->trace("not shown");
    VSphereSoap::Detail::logger()->info("not shown either");
    VSphereSoap::Detail::logger()->warn("shown");

    ASSERT_EQ(m_captured.size(), 1u);
    EXPECT_EQ(m_captured[0].message, "shown");
}

TEST_F(LogTest, OffSilencesEverything)
{
    VSphereSoap::Log::setLevel(Level::Off);

    VSphereSoap::Detail::logger()->error("not shown");

    EXPECT_TRUE(m_captured.empty());
}

TEST(LogLevelNames, MatchTheEnvironmentVariableValues)
{
    EXPECT_EQ(VSphereSoap::Log::toString(Level::Trace), "trace");
    EXPECT_EQ(VSphereSoap::Log::toString(Level::Warn), "warn");
    EXPECT_EQ(VSphereSoap::Log::toString(Level::Off), "off");
}

} // namespace
