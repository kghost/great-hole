#include "AutoHandle.hpp"

#include <gtest/gtest.h>
#include <windows.h>

namespace gh {

TEST(AutoHandleTest, DefaultConstructor) {
  AutoHandle handle;
  EXPECT_FALSE(handle.IsValid());
  EXPECT_FALSE(static_cast<bool>(handle));
  EXPECT_EQ(handle.Get(), nullptr);
}

TEST(AutoHandleTest, ValidHandleLifecycle) {
  HANDLE rawEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ASSERT_NE(rawEvent, nullptr);
  ASSERT_NE(rawEvent, INVALID_HANDLE_VALUE);

  {
    AutoHandle handle(rawEvent);
    EXPECT_TRUE(handle.IsValid());
    EXPECT_TRUE(static_cast<bool>(handle));
    EXPECT_EQ(handle.Get(), rawEvent);
  }
  // handle goes out of scope and closes rawEvent.
}

TEST(AutoHandleTest, MoveSemantics) {
  HANDLE rawEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ASSERT_NE(rawEvent, nullptr);

  AutoHandle h1(rawEvent);
  EXPECT_TRUE(h1.IsValid());

  // Move construct
  AutoHandle h2(std::move(h1));
  EXPECT_FALSE(h1.IsValid());
  EXPECT_EQ(h1.Get(), nullptr);
  EXPECT_TRUE(h2.IsValid());
  EXPECT_EQ(h2.Get(), rawEvent);

  // Move assign
  AutoHandle h3;
  h3 = std::move(h2);
  EXPECT_FALSE(h2.IsValid());
  EXPECT_TRUE(h3.IsValid());
  EXPECT_EQ(h3.Get(), rawEvent);
}

TEST(AutoHandleTest, Release) {
  HANDLE rawEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ASSERT_NE(rawEvent, nullptr);

  AutoHandle handle(rawEvent);
  HANDLE released = handle.Release();

  EXPECT_EQ(released, rawEvent);
  EXPECT_FALSE(handle.IsValid());
  EXPECT_EQ(handle.Get(), nullptr);

  // Clean up manually since ownership was released
  CloseHandle(released);
}

TEST(AutoHandleTest, ResetAndClose) {
  HANDLE event1 = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE event2 = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ASSERT_NE(event1, nullptr);
  ASSERT_NE(event2, nullptr);

  AutoHandle handle(event1);
  EXPECT_EQ(handle.Get(), event1);

  // Reset closes event1 and takes event2
  handle.Reset(event2);
  EXPECT_EQ(handle.Get(), event2);

  // Close explicitly
  handle.Close();
  EXPECT_FALSE(handle.IsValid());
  EXPECT_EQ(handle.Get(), nullptr);
}

TEST(AutoHandleTest, Put) {
  AutoHandle handle;
  HANDLE* pHandle = handle.Put();
  ASSERT_NE(pHandle, nullptr);

  *pHandle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  EXPECT_TRUE(handle.IsValid());
  EXPECT_NE(handle.Get(), nullptr);
}

TEST(AutoHandleTest, InvalidHandleValueHandling) {
  AutoHandle handle(INVALID_HANDLE_VALUE);
  EXPECT_FALSE(handle.IsValid());
  EXPECT_FALSE(static_cast<bool>(handle));
  EXPECT_EQ(handle.Get(), INVALID_HANDLE_VALUE);

  // Close should not fail or throw on INVALID_HANDLE_VALUE
  handle.Close();
  EXPECT_EQ(handle.Get(), nullptr);
  EXPECT_FALSE(handle.IsValid());
}

} // namespace gh
