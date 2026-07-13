#include <variant>

#include <gtest/gtest.h>

#include "PolicyRegistry.hpp"

using namespace gh;
using namespace gh::policy;

class TestPolicyRegistry : public ::testing::Test {
protected:
  PolicyRegistry registry;

  void SetUp() override {
    registry.Clear();
  }

  void TearDown() override {
    registry.Clear();
  }
};

TEST_F(TestPolicyRegistry, RegistryPathMatching) {
  PolicyRegistry& reg = registry;

  PolicyRule bypassRule{.Action = PolicyRule::ByPassRoute{}, .Scope = PolicyScope::SingleProcess};
  PolicyRule discardRule{.Action = PolicyRule::EndpointRoute{}, .Scope = PolicyScope::ProcessSubtree};

  reg.AddPathRule("C:\\Windows\\notepad.exe", bypassRule);
  reg.AddPathRule("C:\\Program Files\\Git\\bin\\git.exe", discardRule);

  auto match1 = reg.GetRuleForPath("C:\\Windows\\notepad.exe");
  ASSERT_TRUE(match1.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::ByPassRoute>(match1->Action));
  EXPECT_EQ(match1->Scope, PolicyScope::SingleProcess);

  auto match3 = reg.GetRuleForPath("C:\\Program Files\\Git\\bin\\git.exe");
  ASSERT_TRUE(match3.has_value());
  EXPECT_TRUE(std::holds_alternative<PolicyRule::EndpointRoute>(match3->Action));
  EXPECT_EQ(match3->Scope, PolicyScope::ProcessSubtree);

  auto match4 = reg.GetRuleForPath("C:\\Windows\\explorer.exe");
  EXPECT_FALSE(match4.has_value());
}
