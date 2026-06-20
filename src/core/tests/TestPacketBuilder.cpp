#include <array>
#include <cstdint>
#include <span>

#include <gtest/gtest.h>

#include "../PacketBuilder.hpp"

namespace {

using namespace gh;

enum class TestEnum : uint8_t { kValA = 0x12, kValB = 0x34 };

// Aliases
using FieldU16 = PacketFieldNumeric<uint16_t>;
using FieldU32 = PacketFieldNumeric<uint32_t>;
using FieldEnum = PacketFieldEnum<TestEnum>;
using FieldRaw = PacketFieldRaw<4>;

struct AliasTag {};
using FieldAlias = PacketFieldAlias<AliasTag, FieldU16>;

TEST(PacketFieldTest, BasicFields) {
  std::array<uint8_t, 16> buffer{};
  std::span<uint8_t> dataSpan(buffer);

  // Numeric field
  FieldU16::Set(dataSpan.subspan<0, 2>(), 0x1234);
  EXPECT_EQ(buffer[0], 0x12);
  EXPECT_EQ(buffer[1], 0x34);
  EXPECT_EQ(FieldU16::Get(dataSpan.subspan<0, 2>()), 0x1234);

  // Enum field
  FieldEnum::Set(dataSpan.subspan<2, 1>(), TestEnum::kValB);
  EXPECT_EQ(buffer[2], 0x34);
  EXPECT_EQ(FieldEnum::Get(dataSpan.subspan<2, 1>()), TestEnum::kValB);

  // Raw field
  std::array<uint8_t, 4> rawData = {0xaa, 0xbb, 0xcc, 0xdd};
  FieldRaw::Set(dataSpan.subspan<3, 4>(), std::span<const uint8_t, 4>(rawData));
  EXPECT_EQ(buffer[3], 0xaa);
  EXPECT_EQ(buffer[4], 0xbb);
  EXPECT_EQ(buffer[5], 0xcc);
  EXPECT_EQ(buffer[6], 0xdd);
  auto retrievedRaw = FieldRaw::Get(dataSpan.subspan<3, 4>());
  EXPECT_EQ(retrievedRaw[0], 0xaa);
  EXPECT_EQ(retrievedRaw[3], 0xdd);

  // Alias field
  FieldAlias::Set(dataSpan.subspan<7, 2>(), 0x5678);
  EXPECT_EQ(buffer[7], 0x56);
  EXPECT_EQ(buffer[8], 0x78);
  EXPECT_EQ(FieldAlias::Get(dataSpan.subspan<7, 2>()), 0x5678);
}

// Containers
using TestSimpleComponent = PacketComponentContainer<std::tuple<FieldU16, FieldEnum, FieldRaw>, PacketComponentEnd>;

TEST(PacketComponentContainerTest, SimpleBuildParse) {
  std::array<uint8_t, 7> buffer{};
  std::span<uint8_t> dataSpan(buffer);

  EXPECT_FALSE(TestSimpleComponent::Validate(dataSpan.first<6>()));

  // Write elements
  std::array<uint8_t, 4> rawVal = {0x01, 0x02, 0x03, 0x04};
  TestSimpleComponent::Set(dataSpan.subspan<0, TestSimpleComponent::BuilderDataSize>(), 0xabcd, TestEnum::kValA,
                           std::span<const uint8_t, 4>(rawVal));

  EXPECT_TRUE(TestSimpleComponent::Validate(dataSpan));

  EXPECT_EQ(buffer[0], 0xab);
  EXPECT_EQ(buffer[1], 0xcd);
  EXPECT_EQ(buffer[2], 0x12);
  EXPECT_EQ(buffer[3], 0x01);
  EXPECT_EQ(buffer[6], 0x04);

  // Parse elements
  PacketParser<TestSimpleComponent, 0> parser(dataSpan);
  EXPECT_EQ(parser.Get<FieldU16>(), 0xabcd);
  EXPECT_EQ(parser.Get<FieldEnum>(), TestEnum::kValA);
  auto rawOut = parser.Get<FieldRaw>();
  EXPECT_EQ(rawOut[0], 0x01);
  EXPECT_EQ(rawOut[3], 0x04);
}

// Chained Components
using ComponentPartB = PacketComponentContainer<std::tuple<FieldRaw>, PacketComponentEnd>;
using ComponentPartA = PacketComponentContainer<std::tuple<FieldU16, FieldEnum>, ComponentPartB>;

TEST(PacketComponentContainerTest, ChainedBuildParse) {
  std::array<uint8_t, 7> buffer{};
  std::span<uint8_t> dataSpan(buffer);

  // Build via wrapper chain
  auto builder = PacketBuilder<ComponentPartA, 0>(dataSpan);
  std::array<uint8_t, 4> rawVal = {0x99, 0x88, 0x77, 0x66};

  // Call operator() on builder for ComponentPartA, then on builder for ComponentPartB
  builder(0x7788, TestEnum::kValB)(std::span<const uint8_t, 4>(rawVal));

  EXPECT_TRUE(ComponentPartA::Validate(dataSpan));

  // Parse
  auto parserA = PacketParser<ComponentPartA, 0>(dataSpan);
  ASSERT_TRUE(ComponentPartA::Validate(dataSpan));

  EXPECT_EQ(parserA.Get<FieldU16>(), 0x7788);
  EXPECT_EQ(parserA.Get<FieldEnum>(), TestEnum::kValB);

  // ComponentPartB starts at offset 3
  PacketParser<ComponentPartB, ComponentPartA::BuilderDataSize> parserB(dataSpan);
  auto rawOut = parserB.Get<FieldRaw>();
  EXPECT_EQ(rawOut[0], 0x99);
  EXPECT_EQ(rawOut[3], 0x66);
}

// Dynamic Enum Maps
using MsgBodyA = PacketComponentContainer<std::tuple<FieldU16>, PacketComponentEnd>;
using MsgBodyB = PacketComponentContainer<std::tuple<FieldU32, FieldRaw>, PacketComponentEnd>;

using TestDynamicPacket = PacketComponentEnumMap<PacketComponentEnumMapEntry<TestEnum::kValA, MsgBodyA>,
                                                 PacketComponentEnumMapEntry<TestEnum::kValB, MsgBodyB>>;

TEST(PacketComponentEnumMapTest, DynamicBuildParse) {
  std::array<uint8_t, 12> buffer{};
  std::span<uint8_t> dataSpan(buffer);

  // 1. Build EntryA
  {
    PacketBuilder<TestDynamicPacket, 0>{dataSpan}(std::integral_constant<TestEnum, TestEnum::kValA>())(0x5566);

    EXPECT_TRUE(TestDynamicPacket::Validate(dataSpan));

    // Buffer structure: [1-byte Enum][2-byte MsgBodyA (FieldU16)] = 3 bytes used
    EXPECT_EQ(buffer[0], static_cast<uint8_t>(TestEnum::kValA));
    EXPECT_EQ(buffer[1], 0x55);
    EXPECT_EQ(buffer[2], 0x66);

    // Parse MsgBodyA
    ASSERT_TRUE(TestDynamicPacket::Validate(dataSpan));
    auto parser = PacketParser<TestDynamicPacket, 0>{dataSpan};

    auto subParserA = parser.As<MsgBodyA>();
    ASSERT_TRUE(subParserA.has_value());
    EXPECT_EQ(subParserA->Get<FieldU16>(), 0x5566);

    auto subParserB = parser.As<MsgBodyB>();
    EXPECT_FALSE(subParserB.has_value());
  }

  // 2. Build EntryB
  {
    std::array<uint8_t, 4> rawVal = {0xee, 0xff, 0xaa, 0xbb};

    PacketBuilder<TestDynamicPacket, 0>{dataSpan}(std::integral_constant<TestEnum, TestEnum::kValB>())(
        uint32_t(0x11223344), std::span<const uint8_t, 4>(rawVal));

    EXPECT_TRUE(TestDynamicPacket::Validate(dataSpan));

    // Buffer structure: [1-byte Enum][4-byte FieldU32][4-byte FieldRaw] = 9 bytes used
    EXPECT_EQ(buffer[0], static_cast<uint8_t>(TestEnum::kValB));
    EXPECT_EQ(buffer[1], 0x11);
    EXPECT_EQ(buffer[4], 0x44);
    EXPECT_EQ(buffer[5], 0xee);
    EXPECT_EQ(buffer[8], 0xbb);

    // Parse MsgBodyB
    ASSERT_TRUE(TestDynamicPacket::Validate(dataSpan));
    auto parser = PacketParser<TestDynamicPacket, 0>{dataSpan};

    auto subParserB = parser.As<MsgBodyB>();
    ASSERT_TRUE(subParserB.has_value());
    EXPECT_EQ(subParserB->Get<FieldU32>(), 0x11223344);
    auto rawOut = subParserB->Get<FieldRaw>();
    EXPECT_EQ(rawOut[0], 0xee);
    EXPECT_EQ(rawOut[3], 0xbb);

    auto subParserA = parser.As<MsgBodyA>();
    EXPECT_FALSE(subParserA.has_value());
  }
}

} // namespace
