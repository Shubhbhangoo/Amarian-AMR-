/// \file
/// Tests for scheme lifecycle (Phase 8): activation, deprecation, retirement,
/// and emergency migration.

#include <amarian/consensus/scheme_lifecycle.hpp>
#include <amarian/crypto/signature.hpp>

#include <gtest/gtest.h>

#include <cstdint>

namespace amarian::consensus {
namespace {

TEST(SchemeLifecycle, AllRegisteredSchemesActiveFromGenesis) {
    SchemeLifecycleRegistry registry;
    for (const auto& spec : crypto::KnownSchemes()) {
        const SchemeState state = registry.GetState(spec.id, 0);
        EXPECT_EQ(state, SchemeState::Active)
            << "scheme " << spec.id << " should be active at height 0";
    }
}

TEST(SchemeLifecycle, ReservedSchemeIsAlwaysRetired) {
    SchemeLifecycleRegistry registry;
    EXPECT_EQ(registry.GetState(0, 0), SchemeState::Retired);
    EXPECT_EQ(registry.GetState(0, 999999), SchemeState::Retired);
    EXPECT_FALSE(registry.IsSpendable(0, 0));
    EXPECT_FALSE(registry.MayCreateOutput(0, 0));
}

TEST(SchemeLifecycle, UnknownSchemeIsPending) {
    SchemeLifecycleRegistry registry;
    EXPECT_EQ(registry.GetState(99, 0), SchemeState::Pending);
    EXPECT_FALSE(registry.MayCreateOutput(99, 0));
    EXPECT_TRUE(registry.IsSpendable(99, 0));
}

TEST(SchemeLifecycle, ActivationAtSpecifiedHeight) {
    SchemeLifecycleRegistry registry;
    registry.Register(SchemeLifecycle{.scheme_id = 99, .activation_height = 1000,
        .deprecation_height = 0xFFFFFFFFU, .retirement_height = 0xFFFFFFFFU});
    EXPECT_EQ(registry.GetState(99, 999), SchemeState::Pending);
    EXPECT_FALSE(registry.MayCreateOutput(99, 999));
    EXPECT_TRUE(registry.IsSpendable(99, 999));
    EXPECT_EQ(registry.GetState(99, 1000), SchemeState::Active);
    EXPECT_TRUE(registry.MayCreateOutput(99, 1000));
    EXPECT_TRUE(registry.IsSpendable(99, 1000));
}

TEST(SchemeLifecycle, DeprecationAllowsSpendsButNotNewOutputs) {
    SchemeLifecycleRegistry registry;
    registry.Register(SchemeLifecycle{.scheme_id = 99, .activation_height = 0,
        .deprecation_height = 5000, .retirement_height = 0xFFFFFFFFU});
    EXPECT_EQ(registry.GetState(99, 4999), SchemeState::Active);
    EXPECT_TRUE(registry.MayCreateOutput(99, 4999));
    EXPECT_TRUE(registry.IsSpendable(99, 4999));
    EXPECT_EQ(registry.GetState(99, 5000), SchemeState::Deprecated);
    EXPECT_FALSE(registry.MayCreateOutput(99, 5000));
    EXPECT_TRUE(registry.IsSpendable(99, 5000));
}

TEST(SchemeLifecycle, RetirementPreventsNewOutputs) {
    SchemeLifecycleRegistry registry;
    registry.Register(SchemeLifecycle{.scheme_id = 99, .activation_height = 0,
        .deprecation_height = 0xFFFFFFFFU, .retirement_height = 10000});
    EXPECT_EQ(registry.GetState(99, 9999), SchemeState::Active);
    EXPECT_TRUE(registry.MayCreateOutput(99, 9999));
    EXPECT_EQ(registry.GetState(99, 10000), SchemeState::Retired);
    EXPECT_FALSE(registry.MayCreateOutput(99, 10000));
    EXPECT_TRUE(registry.IsSpendable(99, 10000));
}

TEST(SchemeLifecycle, RetirementNeverInvalidatesExistingUtxos) {
    SchemeLifecycleRegistry registry;
    registry.Register(SchemeLifecycle{.scheme_id = 99, .activation_height = 0,
        .deprecation_height = 0xFFFFFFFFU, .retirement_height = 10000});
    EXPECT_TRUE(registry.IsSpendable(99, 100000));
    EXPECT_TRUE(registry.IsSpendable(99, 0xFFFFFFFFU));
}

TEST(SchemeLifecycle, FullLifecycle) {
    SchemeLifecycleRegistry registry;
    registry.Register(SchemeLifecycle{.scheme_id = 99, .activation_height = 1000,
        .deprecation_height = 5000, .retirement_height = 10000});
    EXPECT_EQ(registry.GetState(99, 0), SchemeState::Pending);
    EXPECT_FALSE(registry.MayCreateOutput(99, 0));
    EXPECT_TRUE(registry.IsSpendable(99, 0));
    EXPECT_EQ(registry.GetState(99, 1000), SchemeState::Active);
    EXPECT_TRUE(registry.MayCreateOutput(99, 1000));
    EXPECT_TRUE(registry.IsSpendable(99, 1000));
    EXPECT_EQ(registry.GetState(99, 5000), SchemeState::Deprecated);
    EXPECT_FALSE(registry.MayCreateOutput(99, 5000));
    EXPECT_TRUE(registry.IsSpendable(99, 5000));
    EXPECT_EQ(registry.GetState(99, 10000), SchemeState::Retired);
    EXPECT_FALSE(registry.MayCreateOutput(99, 10000));
    EXPECT_TRUE(registry.IsSpendable(99, 10000));
}

TEST(SchemeLifecycle, EmergencyMigrationAcceleratesRetirement) {
    SchemeLifecycleRegistry registry;
    registry.Register(SchemeLifecycle{.scheme_id = 1, .activation_height = 0,
        .deprecation_height = 50000, .retirement_height = 100000});
    registry.Register(SchemeLifecycle{.scheme_id = 99, .activation_height = 100000,
        .deprecation_height = 0xFFFFFFFFU, .retirement_height = 0xFFFFFFFFU});
    EXPECT_EQ(registry.GetState(1, 99999), SchemeState::Deprecated);
    EXPECT_FALSE(registry.MayCreateOutput(1, 99999));
    EXPECT_EQ(registry.GetState(99, 99999), SchemeState::Pending);
    EXPECT_EQ(registry.GetState(1, 100000), SchemeState::Retired);
    EXPECT_FALSE(registry.MayCreateOutput(1, 100000));
    EXPECT_TRUE(registry.IsSpendable(1, 100000));
    EXPECT_EQ(registry.GetState(99, 100000), SchemeState::Active);
    EXPECT_TRUE(registry.MayCreateOutput(99, 100000));
}

TEST(SchemeLifecycle, MultipleSchemesAreIndependent) {
    SchemeLifecycleRegistry registry;
    registry.Register(SchemeLifecycle{.scheme_id = 99, .activation_height = 100,
        .deprecation_height = 0xFFFFFFFFU, .retirement_height = 0xFFFFFFFFU});
    registry.Register(SchemeLifecycle{.scheme_id = 98, .activation_height = 200,
        .deprecation_height = 0xFFFFFFFFU, .retirement_height = 0xFFFFFFFFU});
    EXPECT_EQ(registry.GetState(99, 150), SchemeState::Active);
    EXPECT_EQ(registry.GetState(98, 150), SchemeState::Pending);
    EXPECT_EQ(registry.GetState(98, 250), SchemeState::Active);
}

TEST(SchemeLifecycle, AllKnownSchemesAreCreatableAndSpendableAtGenesis) {
    SchemeLifecycleRegistry registry;
    for (const auto& spec : crypto::KnownSchemes()) {
        EXPECT_TRUE(registry.MayCreateOutput(spec.id, 0))
            << "scheme " << spec.id << " should be creatable at genesis";
        EXPECT_TRUE(registry.IsSpendable(spec.id, 0))
            << "scheme " << spec.id << " should be spendable at genesis";
    }
}

}  // namespace
}  // namespace amarian::consensus