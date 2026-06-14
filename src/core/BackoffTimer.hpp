#pragma once

#include <chrono>
#if defined(__has_include) && __has_include(<generator>)
#include <generator>
#else
#include <coroutine>
#include <iterator>
#include <utility>

namespace std {

template <typename T>
class generator {
public:
  struct promise_type {
    const T* value = nullptr;

    generator get_return_object() {
      return generator(std::coroutine_handle<promise_type>::from_promise(*this));
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    std::suspend_always yield_value(const T& val) noexcept {
      value = std::addressof(val);
      return {};
    }
    void return_void() noexcept {}
    void unhandled_exception() { throw; }
  };

  using handle_type = std::coroutine_handle<promise_type>;

  explicit generator(handle_type h) : _Coro(h) {}
  ~generator() {
    if (_Coro) {
      _Coro.destroy();
    }
  }

  generator(const generator&) = delete;
  generator& operator=(const generator&) = delete;
  generator(generator&& other) noexcept : _Coro(other._Coro) {
    other._Coro = nullptr;
  }
  generator& operator=(generator&& other) noexcept {
    if (this != &other) {
      if (_Coro) {
        _Coro.destroy();
      }
      _Coro = other._Coro;
      other._Coro = nullptr;
    }
    return *this;
  }

  class iterator {
  public:
    using difference_type = std::ptrdiff_t;
    using value_type = T;
    using pointer = const T*;
    using reference = const T&;
    using iterator_category = std::input_iterator_tag;

    iterator() = default;
    explicit iterator(handle_type h) : _Coro(h) {}

    iterator& operator++() {
      _Coro.resume();
      return *this;
    }
    void operator++(int) {
      _Coro.resume();
    }
    const T& operator*() const {
      return *_Coro.promise().value;
    }
    bool operator==(const iterator& other) const {
      return _Coro == other._Coro || (_Coro && _Coro.done() && !other._Coro);
    }
    bool operator!=(const iterator& other) const {
      return !(*this == other);
    }

  private:
    handle_type _Coro;
  };

  iterator begin() {
    if (_Coro) {
      _Coro.resume();
    }
    return iterator(_Coro);
  }

  iterator end() {
    return iterator(nullptr);
  }

private:
  handle_type _Coro;
};

} // namespace std
#endif

namespace gh {

template <typename Generator> class GeneratorHelper {
public:
  using ValueType = std::ranges::range_value_t<Generator>;
  using IteratorType = std::ranges::iterator_t<Generator>;

  explicit GeneratorHelper(Generator generator) : _Generator(std::move(generator)), _Iterator(_Generator.begin()) {}

  ValueType operator()() {
    auto value = std::move(*_Iterator);
    ++_Iterator;
    return value;
  }
  bool hasNext() const { return _Iterator != _Generator.end(); }

private:
  Generator _Generator;
  IteratorType _Iterator;
};

GeneratorHelper<std::generator<std::chrono::milliseconds>> BackoffTimerDuration(int randomnessPercent,
                                                                                std::chrono::milliseconds current,
                                                                                std::chrono::milliseconds step,
                                                                                std::chrono::milliseconds maximum);

} // namespace gh