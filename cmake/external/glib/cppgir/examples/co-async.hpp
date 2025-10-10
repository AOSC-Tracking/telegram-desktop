#pragma once

#include <gi/gi.hpp>

#include <coroutine>
#include <future>

#ifdef CO_DEBUG
#include <iostream>
static auto &dout = std::cerr;
#else
#include <sstream>
static std::ostringstream dout;
#endif

template<typename T, typename SELF>
struct holder
{
  void return_value(T &&v)
  {
    dout << "return value " << std::endl;
    auto self = (SELF *)this;
    self->set_value(std::move(v));
  }
};

template<typename SELF>
struct holder<void, SELF>
{
  void return_void()
  {
    auto self = (SELF *)this;
    self->set_value();
  }
};

template<typename RESULT>
class promise_type_t : public holder<RESULT, promise_type_t<RESULT>>
{
protected:
  std::promise<RESULT> result_;
  std::coroutine_handle<> waiter_;

public:
  struct init
  {
    std::coroutine_handle<> handle;
    std::future<RESULT> f;
  };

  ~promise_type_t() { dout << "promise destruction" << std::endl; }

  auto get_return_object(bool refresh = false)
  {
    dout << "return obj " << std::endl;
    if (refresh)
      result_ = decltype(result_)();
    return init{std::coroutine_handle<promise_type_t>::from_promise(*this),
        result_.get_future()};
  }
  std::suspend_never initial_suspend() noexcept { return {}; }
  std::suspend_never final_suspend() noexcept { return {}; }

  bool resume()
  {
    auto w = waiter_;
    if (w) {
      // waiter takes care of itself again
      waiter_ = nullptr;
      w.resume();
    }
    return bool(w);
  }

  // use any dummy type to avoid reference to void below
  using arg_type = typename std::conditional<std::is_same<RESULT, void>::value,
      std::nullptr_t, RESULT>::type;

  void set_value(arg_type &&v)
  {
    result_.set_value(std::move(v));
    resume();
  }

  void set_value()
  {
    result_.set_value();
    resume();
  }

  void set_waiter(std::coroutine_handle<> handle)
  {
    // a task/promise represent a coroutine function (frame)
    // it should only be waited upon by one other task
    // (rather than handed around and waited in multiple locations)
    if (waiter_)
      gi::detail::try_throw(std::logic_error("already waited upon"));
    waiter_ = handle;
  }

  void unhandled_exception()
  {
#if GI_CONFIG_EXCEPTIONS
    result_.set_exception(std::current_exception());
    // if no-one waiting, deliver to caller
    // the latter likely is the original caller
    // (to which we have not yet returned, so it can yet await)
    // otherwise it might end up totally lost
    if (!resume())
      throw;
#endif
  }
};

template<typename RESULT, typename P = promise_type_t<RESULT>>
class task
{
public:
  using promise_type = P;

  // only 1 actually active
  std::coroutine_handle<promise_type> coro_;
  std::unique_ptr<promise_type> p_;
  // but always this
  std::future<RESULT> result_;

public:
  // NOTE if coroutine exits by co_return, then handle is not useful
  // but the future should have a value
  task(typename P::init i)
      : coro_(decltype(coro_)::from_address(i.handle.address())),
        result_(std::move(i.f))
  {
    dout << "init task" << std::endl;
  }

  task() : p_(new promise_type()) { result_ = p_->get_return_object().f; }

  // move-only
  task(task &&other) = default;
  task &operator=(task &&other) = delete;

  bool await_ready()
  {
    return result_.wait_for(std::chrono::seconds(0)) ==
           std::future_status::ready;
  }

  promise_type &promise() const { return coro_ ? coro_.promise() : *p_; }

  void await_suspend(std::coroutine_handle<> handle)
  {
    if (!coro_ && !p_)
      gi::detail::try_throw(std::logic_error("no routine to wait on"));
    promise().set_waiter(handle);
  }

  RESULT await_resume() { return result_.get(); }
};
