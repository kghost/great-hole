#include "StateMachine.hpp"

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

#include "Asio.hpp"
#include "Coroutine.hpp"
#include "Manager.hpp"
#include "Utils/Overload.hpp"

namespace gh {
namespace {

enum class States : uint8_t {
  InitialState,
  State1,
  State2,
  State3,
};

struct ActionInitTo1 {
  int value{0};
};

struct Action1To2 {
  std::string name;
};

struct Action1To3 {
  double count{0.0};
};

struct Action2To1 {
  int resetVal{0};
};

struct Nothing {};

struct StateData1 {
  explicit StateData1(ActionInitTo1 arg) : val(arg.value) {}
  explicit StateData1(Action2To1 arg) : val(arg.resetVal) {}
  int val{0};
};

struct StateData2 {
  explicit StateData2(Action1To2 arg) : text(std::move(arg.name)) {}
  std::string text;
};

struct StateData3 {
  explicit StateData3(Action1To3 arg) : num(arg.count) {}
  double num{0.0};
};

using TestMachine =
    StateMachine<States::InitialState,
                 StateDefines<StateDefine<States::InitialState, Nothing>, StateDefine<States::State1, StateData1>,
                              StateDefine<States::State2, StateData2>, StateDefine<States::State3, StateData3>>,
                 Transitions<Transition<States::InitialState, ActionInitTo1, States::State1>,
                             Transition<States::State1, Action1To2, States::State2>,
                             Transition<States::State1, Action1To3, States::State3>,
                             Transition<States::State2, Action2To1, States::State1>>>;

void RunFiberTest(std::function<Omni::Fiber::Coroutine<void>()> fn) {
  boost::asio::io_context io;
  Omni::Fiber::AsioExecutor executor(io.get_executor());
  Omni::Fiber::Manager manager(executor);
  manager.SpawnRoot("root", std::move(fn));
  io.restart();
  io.run();
}

TEST(StateMachineTest, InitialState) {
  TestMachine machine;

  EXPECT_EQ(machine.GetState(), States::InitialState);
  EXPECT_TRUE(machine.IsState<States::InitialState>());
  EXPECT_FALSE(machine.IsState<States::State1>());

  EXPECT_NO_THROW(machine.GetData<States::InitialState>());
  EXPECT_THROW(machine.GetData<States::State1>(), std::runtime_error);
}

TEST(StateMachineTest, SingleActionTransition) {
  RunFiberTest([&]() -> Omni::Fiber::Coroutine<void> {
    TestMachine machine;

    co_await machine.Action(
        Overload{[](std::integral_constant<States, States::InitialState>,
                    Nothing&) -> TestMachine::ActionResult<States::InitialState> { return ActionInitTo1{.value = 42}; },
                 [](auto tag, auto&) {
                   using Tag = decltype(tag);
                   ADD_FAILURE() << "Should not be called";
                   return TestMachine::ActionResult<Tag::value>{};
                 }});

    EXPECT_EQ(machine.GetState(), States::State1);
    EXPECT_TRUE(machine.IsState<States::State1>());

    EXPECT_EQ(machine.GetData<States::State1>().val, 42);
    co_return;
  });
}

TEST(StateMachineTest, VariantActionResultTransition) {
  RunFiberTest([&]() -> Omni::Fiber::Coroutine<void> {
    TestMachine machine;

    // Transition from InitialState to State1
    co_await machine.Action(
        Overload{[](std::integral_constant<States, States::InitialState>,
                    Nothing&) -> TestMachine::ActionResult<States::InitialState> { return ActionInitTo1{.value = 100}; },
                 [](auto tag, auto&) {
                   using Tag = decltype(tag);
                   return TestMachine::ActionResult<Tag::value>{};
                 }});

    EXPECT_EQ(machine.GetState(), States::State1);

    // Transition from State1 to State3 using variant ActionResult
    co_await machine.Action(Overload{[](std::integral_constant<States, States::State1>,
                               StateData1& data) -> TestMachine::ActionResult<States::State1> {
                              EXPECT_EQ(data.val, 100);
                              return Action1To3{.count = 3.14};
                            },
                            [](auto tag, auto&) {
                              using Tag = decltype(tag);
                              ADD_FAILURE() << "Unexpected state call";
                              return TestMachine::ActionResult<Tag::value>{};
                            }});

    EXPECT_EQ(machine.GetState(), States::State3);
    EXPECT_TRUE(machine.IsState<States::State3>());

    const auto& data3 = machine.GetData<States::State3>();
    EXPECT_DOUBLE_EQ(data3.num, 3.14);
    co_return;
  });
}

TEST(StateMachineTest, DataDestructionAndConstruction) {
  RunFiberTest([&]() -> Omni::Fiber::Coroutine<void> {
    static int dtorsRun = 0;

    struct TrackDtor {
      explicit TrackDtor(ActionInitTo1 arg) { (void)arg; }
      ~TrackDtor() { dtorsRun++; }
      TrackDtor(const TrackDtor&) = delete;
      auto operator=(const TrackDtor&) -> TrackDtor& = delete;
      TrackDtor(TrackDtor&&) noexcept = default;
      auto operator=(TrackDtor&&) noexcept -> TrackDtor& = default;
    };

    using DtorMachine =
        StateMachine<States::InitialState,
                     StateDefines<StateDefine<States::InitialState, Nothing>, StateDefine<States::State1, TrackDtor>,
                                  StateDefine<States::State2, StateData2>>,
                     Transitions<Transition<States::InitialState, ActionInitTo1, States::State1>,
                                 Transition<States::State1, Action1To2, States::State2>>>;

    dtorsRun = 0;
    {
      DtorMachine machine;
      EXPECT_EQ(dtorsRun, 0);

      co_await machine.Action([](std::integral_constant<States, States::InitialState>, Nothing&) -> ActionInitTo1 {
        return ActionInitTo1{};
      });
      EXPECT_EQ(machine.GetState(), States::State1);
      EXPECT_EQ(dtorsRun, 0);

      // Transition from State1 to State2 should destroy TrackDtor
      co_await machine.Action([](std::integral_constant<States, States::State1>, TrackDtor&) -> Action1To2 {
        return Action1To2{.name = "hello"};
      });
      EXPECT_EQ(machine.GetState(), States::State2);
      EXPECT_EQ(dtorsRun, 1);
    }
    co_return;
  });
}

TEST(StateMachineTest, CoroutineVisitorTransition) {
  RunFiberTest([&]() -> Omni::Fiber::Coroutine<void> {
    TestMachine machine;

    co_await machine.Action(
        [](std::integral_constant<States, States::InitialState>,
           Nothing&) -> Omni::Fiber::Coroutine<TestMachine::ActionResult<States::InitialState>> {
          co_return ActionInitTo1{.value = 88};
        });

    EXPECT_EQ(machine.GetState(), States::State1);
    EXPECT_EQ(machine.GetData<States::State1>().val, 88);
    co_return;
  });
}

TEST(StateMachineTest, StateTypeConstraintCheck) {
  enum class OtherEnum : uint8_t { Val1, Val2 };

  using ValidDefs = StateDefines<StateDefine<States::InitialState, Nothing>, StateDefine<States::State1, StateData1>>;
  using InvalidDefs = StateDefines<StateDefine<OtherEnum::Val1, Nothing>>;

  static_assert(ValidStateDefinesFor<ValidDefs, States>);
  static_assert(!ValidStateDefinesFor<InvalidDefs, States>);

  using ValidTransPack = Transitions<Transition<States::InitialState, ActionInitTo1, States::State1>>;
  using InvalidTransPack = Transitions<Transition<OtherEnum::Val1, ActionInitTo1, States::State1>>;

  static_assert(ValidTransitionsFor<ValidTransPack, States>);
  static_assert(!ValidTransitionsFor<InvalidTransPack, States>);

  struct NonConstructibleData {};
  using NonConstructibleDefs =
      StateDefines<StateDefine<States::InitialState, Nothing>, StateDefine<States::State1, NonConstructibleData>>;
  static_assert(!ValidTransitionsConstructible<States::InitialState, NonConstructibleDefs, ValidTransPack>);
}

} // namespace
} // namespace gh
