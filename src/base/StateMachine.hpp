#pragma once

#include <array>
#include <cstddef>
#include <format>
#include <type_traits>
#include <utility>
#include <variant>

#include "Coroutine.hpp"
#include "Mutex.hpp"

namespace gh {

template <auto StateVal, typename Data> struct StateDefine {
  using StateType = decltype(StateVal);
  static constexpr StateType state = StateVal;
  using dataType = Data;
};

template <typename T> struct IsStateDefineSpec : std::false_type {};
template <auto StateVal, typename Data> struct IsStateDefineSpec<StateDefine<StateVal, Data>> : std::true_type {};

template <typename T>
concept IsStateDefine = IsStateDefineSpec<std::decay_t<T>>::value;

template <IsStateDefine... Defs> struct StateDefines {};

template <typename T> struct IsStateDefinesSpec : std::false_type {};
template <IsStateDefine... Defs> struct IsStateDefinesSpec<StateDefines<Defs...>> : std::true_type {};

template <typename T>
concept IsStateDefines = IsStateDefinesSpec<std::decay_t<T>>::value;

template <auto FromStateVal, typename Action, auto ToStateVal> struct Transition {
  using FromStateType = decltype(FromStateVal);
  using ToStateType = decltype(ToStateVal);
  static constexpr auto from = FromStateVal;
  using actionType = Action;
  static constexpr auto toState = ToStateVal;
};

template <typename T> struct IsTransitionSpec : std::false_type {};
template <auto FromStateVal, typename Action, auto ToStateVal>
struct IsTransitionSpec<Transition<FromStateVal, Action, ToStateVal>> : std::true_type {};

template <typename T>
concept IsTransition = IsTransitionSpec<std::decay_t<T>>::value;

template <IsTransition... Trans> struct Transitions {};

template <typename T> struct IsTransitionsSpec : std::false_type {};
template <IsTransition... Trans> struct IsTransitionsSpec<Transitions<Trans...>> : std::true_type {};

template <typename T>
concept IsTransitions = IsTransitionsSpec<std::decay_t<T>>::value;

template <typename T, typename StateType>
concept IsStateDefineFor =
    IsStateDefine<T> && std::is_same_v<std::decay_t<decltype(std::decay_t<T>::state)>, std::decay_t<StateType>>;

template <typename T, typename StateType>
concept IsTransitionFor =
    IsTransition<T> && std::is_same_v<std::decay_t<decltype(std::decay_t<T>::from)>, std::decay_t<StateType>> &&
    std::is_same_v<std::decay_t<decltype(std::decay_t<T>::toState)>, std::decay_t<StateType>>;

namespace detail {

template <typename DefsPack, typename StateType> struct IsValidStateDefinesFor : std::false_type {};
template <typename... Defs, typename StateType>
struct IsValidStateDefinesFor<StateDefines<Defs...>, StateType>
    : std::bool_constant<(IsStateDefineFor<Defs, StateType> && ...)> {};

template <typename TransPack, typename StateType> struct IsValidTransitionsFor : std::false_type {};
template <typename... Trans, typename StateType>
struct IsValidTransitionsFor<Transitions<Trans...>, StateType>
    : std::bool_constant<(IsTransitionFor<Trans, StateType> && ...)> {};

} // namespace detail

template <typename T, typename StateType>
concept ValidStateDefinesFor = detail::IsValidStateDefinesFor<std::decay_t<T>, StateType>::value;

template <typename T, typename StateType>
concept ValidTransitionsFor = detail::IsValidTransitionsFor<std::decay_t<T>, StateType>::value;

namespace detail {

template <auto StateVal, typename StateDefinesPack> struct IndexOfState;

template <auto StateVal, typename... Defs> struct IndexOfState<StateVal, StateDefines<Defs...>> {
private:
  static constexpr auto kStates = std::to_array({Defs::state...});

  static constexpr auto FindIndex() -> std::size_t {
    for (std::size_t i = 0; i < kStates.size(); ++i) {
      if (kStates[i] == StateVal) {
        return i;
      }
    }
    return static_cast<std::size_t>(-1);
  }

public:
  static constexpr std::size_t value = FindIndex();
  static_assert(value != static_cast<std::size_t>(-1), "State value not found in StateDefines!");
};

template <typename... Ts> struct TypeList {};

template <typename List, typename T> struct AppendUnique;

template <typename... Ts, typename T> struct AppendUnique<TypeList<Ts...>, T> {
  using type = std::conditional_t<(std::is_same_v<Ts, T> || ...), TypeList<Ts...>, TypeList<Ts..., T>>;
};

template <auto StateVal, typename TransitionsPack> struct FilterActionsFromState;

template <auto StateVal, typename... Trans> struct FilterActionsFromState<StateVal, Transitions<Trans...>> {
private:
  template <typename Acc, typename... Remaining> struct Helper;

  template <typename Acc> struct Helper<Acc> {
    using type = Acc;
  };

  template <typename Acc, typename Head, typename... Tail> struct Helper<Acc, Head, Tail...> {
    using NextAcc =
        std::conditional_t<Head::from == StateVal, typename AppendUnique<Acc, typename Head::actionType>::type, Acc>;
    using type = typename Helper<NextAcc, Tail...>::type;
  };

public:
  using type = typename Helper<TypeList<>, Trans...>::type;
};

template <typename TypeListInst> struct TypeListToVariant;

template <typename... Actions> struct TypeListToVariant<TypeList<Actions...>> {
  using type = std::conditional_t<(std::is_same_v<Actions, std::monostate> || ...), std::variant<Actions...>,
                                  std::variant<Actions..., std::monostate>>;
};

template <auto FromStateVal, typename Action, typename TransitionsPack> struct FindTargetState;

template <auto FromStateVal, typename Action, typename... Trans>
struct FindTargetState<FromStateVal, Action, Transitions<Trans...>> {
private:
  template <typename... Remaining> struct Helper {
    static constexpr bool found = false;
    static constexpr auto targetState = FromStateVal;
  };

  template <typename Head, typename... Tail> struct Helper<Head, Tail...> {
  private:
    static constexpr bool kMatches = (Head::from == FromStateVal && std::is_same_v<typename Head::actionType, Action>);

  public:
    static constexpr bool found = kMatches ? true : Helper<Tail...>::found;
    static constexpr auto targetState = kMatches ? Head::toState : Helper<Tail...>::targetState;
  };

public:
  static constexpr bool found = Helper<Trans...>::found;
  static constexpr auto targetState = Helper<Trans...>::targetState;
};

template <auto StateVal, typename StateDefinesPack> struct DataTypeForState {
private:
  static constexpr std::size_t idx = IndexOfState<StateVal, StateDefinesPack>::value;

  template <std::size_t Index, typename DefsPack> struct Helper;

  template <std::size_t Index, typename... Defs> struct Helper<Index, StateDefines<Defs...>> {
    using type = std::tuple_element_t<Index, std::tuple<typename Defs::dataType...>>;
  };

public:
  using type = typename Helper<idx, StateDefinesPack>::type;
};

template <auto InitialState, typename StateDefinesPack, typename TransitionsPack> struct AreTransitionsConstructible;

template <auto InitialState, typename StateDefinesPack, typename... Trans>
struct AreTransitionsConstructible<InitialState, StateDefinesPack, Transitions<Trans...>> {
private:
  template <typename Head> static constexpr auto CheckHead() -> bool {
    using TargetData = typename DataTypeForState<Head::toState, StateDefinesPack>::type;
    if constexpr (Head::toState != InitialState) {
      return std::is_constructible_v<TargetData, typename Head::actionType>;
    } else {
      return std::is_constructible_v<TargetData, typename Head::actionType> ||
             std::is_default_constructible_v<TargetData>;
    }
  }

public:
  static constexpr bool value = (sizeof...(Trans) == 0) || (CheckHead<Trans>() && ...);
};

template <typename T> struct IsVariant : std::false_type {};
template <typename... Ts> struct IsVariant<std::variant<Ts...>> : std::true_type {};

template <typename T> constexpr auto ToUnderlyingValue(T val) noexcept {
  if constexpr (std::is_enum_v<T>) {
    return static_cast<std::underlying_type_t<T>>(val);
  } else {
    return val;
  }
}

template <typename ActionType, typename AllowedTypeList> struct IsActionInList : std::false_type {};
template <typename ActionType, typename... AllowedActions>
struct IsActionInList<ActionType, TypeList<AllowedActions...>>
    : std::bool_constant<(std::is_same_v<ActionType, AllowedActions> || ...)> {};

template <typename ResultType, typename AllowedTypeList>
struct IsValidActionResult : IsActionInList<ResultType, AllowedTypeList> {};

template <typename AllowedTypeList> struct IsValidActionResult<void, AllowedTypeList> : std::true_type {};
template <typename AllowedTypeList> struct IsValidActionResult<std::monostate, AllowedTypeList> : std::true_type {};

template <typename... Ts, typename AllowedTypeList>
struct IsValidActionResult<std::variant<Ts...>, AllowedTypeList>
    : std::bool_constant<((std::is_same_v<Ts, std::monostate> || IsActionInList<Ts, AllowedTypeList>::value) && ...)> {
};

} // namespace detail

template <auto InitialState, typename StateDefinesPack, typename TransitionsPack>
concept ValidTransitionsConstructible =
    detail::AreTransitionsConstructible<InitialState, StateDefinesPack, TransitionsPack>::value;

template <auto InitialState, ValidStateDefinesFor<decltype(InitialState)> StateDefinesPack,
          ValidTransitionsFor<decltype(InitialState)> TransitionsPack>
  requires ValidTransitionsConstructible<InitialState, StateDefinesPack, TransitionsPack>
class StateMachine {
private:
  using InitialData = typename detail::DataTypeForState<InitialState, StateDefinesPack>::type;
  static_assert(std::is_default_constructible_v<InitialData>, "InitialState data MUST be default constructible!");
  static_assert(ValidTransitionsConstructible<InitialState, StateDefinesPack, TransitionsPack>,
                "All non-initial StateData types MUST be constructible from their transition Action type!");

public:
  using StateType = decltype(InitialState);

  template <auto S, typename D> using StateDefine = gh::StateDefine<S, D>;

  template <typename... Defs> using StateDefines = gh::StateDefines<Defs...>;

  template <auto From, typename Act, auto To> using Transition = gh::Transition<From, Act, To>;

  template <typename... Trans> using Transitions = gh::Transitions<Trans...>;

  template <auto StateVal>
  using ActionResult = typename detail::TypeListToVariant<
      typename detail::FilterActionsFromState<StateVal, TransitionsPack>::type>::type;

  template <auto StateVal> using CoroutineActionResult = Omni::Fiber::Coroutine<ActionResult<StateVal>>;

private:
  template <typename DefsPack> struct DataVariantFromDefines;

  template <typename... Defs> struct DataVariantFromDefines<gh::StateDefines<Defs...>> {
    using type = std::variant<typename Defs::dataType...>;
  };

  using StorageType = typename DataVariantFromDefines<StateDefinesPack>::type;

  static constexpr std::size_t kInitialIndex = detail::IndexOfState<InitialState, StateDefinesPack>::value;

  Omni::Fiber::Mutex _Mutex;
  StorageType _storage{std::in_place_index<kInitialIndex>};
  std::size_t _stateIndex{kInitialIndex};

  template <typename DefsPack> struct StateArrayFromDefines;

  template <typename... Defs> struct StateArrayFromDefines<gh::StateDefines<Defs...>> {
    static constexpr auto kStates = std::to_array({Defs::state...});
  };

  static constexpr auto kStates = StateArrayFromDefines<StateDefinesPack>::kStates;

public:
  explicit StateMachine() = default;

  auto GetState() const noexcept -> StateType { return kStates[_stateIndex]; }

  template <auto StateVal> [[nodiscard]] auto IsState() const noexcept -> bool { return GetState() == StateVal; }

  template <auto StateVal> auto GetData() -> auto& {
    constexpr std::size_t idx = detail::IndexOfState<StateVal, StateDefinesPack>::value;
    if (_stateIndex == idx) {
      return std::get<idx>(_storage);
    }
    throw std::runtime_error(std::format("State does not match: {} != {}",
                                         detail::ToUnderlyingValue(kStates[_stateIndex]),
                                         detail::ToUnderlyingValue(StateVal)));
  }

  template <auto StateVal> auto GetData() const -> const auto& {
    constexpr std::size_t idx = detail::IndexOfState<StateVal, StateDefinesPack>::value;
    if (_stateIndex == idx) {
      return std::get<idx>(_storage);
    }
    throw std::runtime_error(std::format("State does not match: {} != {}",
                                         detail::ToUnderlyingValue(kStates[_stateIndex]),
                                         detail::ToUnderlyingValue(StateVal)));
  }

  template <typename Visitor> auto Action(Visitor&& visitor) -> Omni::Fiber::Coroutine<void> {
    auto lock = co_await _Mutex.Wait();
    co_await [this, &visitor]<std::size_t... Is>(std::index_sequence<Is...>) -> Omni::Fiber::Coroutine<void> {
      bool handled = false;
      ((_stateIndex == Is ? (co_await ExecuteForIndex<Is>(std::forward<Visitor>(visitor)), handled = true) : false) ||
       ...);
      (void)handled;
    }(std::make_index_sequence<std::variant_size_v<StorageType>>{});
  }

private:
  template <std::size_t Index, typename Visitor>
  auto ExecuteForIndex(Visitor&& visitor) -> Omni::Fiber::Coroutine<void> {
    constexpr StateType currentState = kStates[Index];
    using CurrentData = std::variant_alternative_t<Index, StorageType>;
    CurrentData& currentData = std::get<Index>(_storage);

    using TagType = std::integral_constant<StateType, currentState>;

    if constexpr (std::is_invocable_v<Visitor&, TagType, CurrentData&>) {
      using RawResult = std::invoke_result_t<Visitor&, TagType, CurrentData&>;
      using ActionResType =
          typename Omni::Fiber::CoroutineTraits<std::decay_t<RawResult>>::CoroutineReturnTypeOrOriginalType;
      using AllowedActions = typename detail::FilterActionsFromState<currentState, TransitionsPack>::type;

      static_assert(detail::IsValidActionResult<ActionResType, AllowedActions>::value,
                    "Action result returned by visitor is not defined in transitions for this state!");

      if constexpr (Omni::Fiber::CoroutineTraits<std::decay_t<RawResult>>::value) {
        if constexpr (std::is_void_v<ActionResType>) {
          co_await std::forward<Visitor>(visitor)(TagType{}, currentData);
        } else {
          auto actionResult = co_await std::forward<Visitor>(visitor)(TagType{}, currentData);
          using ResultType = std::decay_t<decltype(actionResult)>;
          if constexpr (detail::IsVariant<ResultType>::value) {
            std::visit(
                [this](auto&& act) -> auto {
                  using ActType = std::decay_t<decltype(act)>;
                  if constexpr (!std::is_same_v<ActType, std::monostate>) {
                    this->ApplyAction<currentState>(std::forward<decltype(act)>(act));
                  }
                },
                actionResult);
          } else {
            this->ApplyAction<currentState>(std::move(actionResult));
          }
        }
      } else {
        if constexpr (std::is_void_v<ActionResType>) {
          std::forward<Visitor>(visitor)(TagType{}, currentData);
        } else {
          auto actionResult = std::forward<Visitor>(visitor)(TagType{}, currentData);
          using ResultType = std::decay_t<decltype(actionResult)>;
          if constexpr (detail::IsVariant<ResultType>::value) {
            std::visit(
                [this](auto&& act) -> auto {
                  using ActType = std::decay_t<decltype(act)>;
                  if constexpr (!std::is_same_v<ActType, std::monostate>) {
                    this->ApplyAction<currentState>(std::forward<decltype(act)>(act));
                  }
                },
                actionResult);
          } else {
            this->ApplyAction<currentState>(std::move(actionResult));
          }
        }
      }
    }
  }

  template <auto CurrentStateVal, typename ActionResult> void ApplyAction(ActionResult&& result) {
    using FoundTarget = detail::FindTargetState<CurrentStateVal, std::decay_t<ActionResult>, TransitionsPack>;
    static_assert(FoundTarget::found, "No valid transition found!");
    constexpr auto targetStateVal = FoundTarget::targetState;
    if constexpr (targetStateVal != CurrentStateVal) {
      constexpr std::size_t targetIndex = detail::IndexOfState<targetStateVal, StateDefinesPack>::value;
      _storage.template emplace<targetIndex>(std::forward<ActionResult>(result));
      _stateIndex = targetIndex;
    }
  }
};

} // namespace gh
