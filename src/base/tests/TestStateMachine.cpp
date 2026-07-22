#include "StateMachine.hpp"

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

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

TEST(StateMachineTest, InitialState) {
  TestMachine machine;

  EXPECT_EQ(machine.GetState(), States::InitialState);
  EXPECT_TRUE(machine.IsState<States::InitialState>());
  EXPECT_FALSE(machine.IsState<States::State1>());

  EXPECT_NE(machine.GetData<States::InitialState>(), nullptr);
  EXPECT_EQ(machine.GetData<States::State1>(), nullptr);
}

TEST(StateMachineTest, SingleActionTransition) {
  TestMachine machine;

  machine.Action(
      Overload{[](std::integral_constant<States, States::InitialState>,
                  Nothing&) -> TestMachine::ActionResult<States::InitialState> { return ActionInitTo1{.value = 42}; },
               [](auto, auto&) -> std::variant<Action1To2, Action1To3, Action2To1> {
                 ADD_FAILURE() << "Should not be called";
                 return Action1To2{};
               }});

  EXPECT_EQ(machine.GetState(), States::State1);
  EXPECT_TRUE(machine.IsState<States::State1>());

  const auto* data1 = machine.GetData<States::State1>();
  ASSERT_NE(data1, nullptr);
  EXPECT_EQ(data1->val, 42);
}

TEST(StateMachineTest, VariantActionResultTransition) {
  TestMachine machine;

  // Transition from InitialState to State1
  machine.Action(
      Overload{[](std::integral_constant<States, States::InitialState>,
                  Nothing&) -> TestMachine::ActionResult<States::InitialState> { return ActionInitTo1{.value = 100}; },
               [](auto, auto&) -> std::variant<Action1To2, Action1To3, Action2To1> { return Action1To2{}; }});

  EXPECT_EQ(machine.GetState(), States::State1);

  // Transition from State1 to State3 using variant ActionResult
  machine.Action(Overload{[](std::integral_constant<States, States::State1>,
                             StateData1& data) -> TestMachine::ActionResult<States::State1> {
                            EXPECT_EQ(data.val, 100);
                            return Action1To3{.count = 3.14};
                          },
                          [](auto, auto&) -> std::variant<ActionInitTo1, Action1To2, Action1To3, Action2To1> {
                            ADD_FAILURE() << "Unexpected state call";
                            return Action1To2{};
                          }});

  EXPECT_EQ(machine.GetState(), States::State3);
  EXPECT_TRUE(machine.IsState<States::State3>());

  const auto* data3 = machine.GetData<States::State3>();
  ASSERT_NE(data3, nullptr);
  EXPECT_DOUBLE_EQ(data3->num, 3.14);
}

TEST(StateMachineTest, DataDestructionAndConstruction) {
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

    machine.Action([](std::integral_constant<States, States::InitialState>, Nothing&) -> ActionInitTo1 {
      return ActionInitTo1{};
    });
    EXPECT_EQ(machine.GetState(), States::State1);
    EXPECT_EQ(dtorsRun, 0);

    // Transition from State1 to State2 should destroy TrackDtor
    machine.Action([](std::integral_constant<States, States::State1>, TrackDtor&) -> Action1To2 {
      return Action1To2{.name = "hello"};
    });
    EXPECT_EQ(machine.GetState(), States::State2);
    EXPECT_EQ(dtorsRun, 1);
  }
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
