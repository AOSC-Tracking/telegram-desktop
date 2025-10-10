#define GI_INLINE 1
#include <gio/gio.hpp>

#include "co-async.hpp"

namespace GLib = gi::repository::GLib;
namespace GObject_ = gi::repository::GObject;
namespace Gio = gi::repository::Gio;

template<typename R>
class async_result_promise_type_t : public promise_type_t<R>
{
public:
  Gio::Cancellable cancel_;
};

template<typename RESULT = Gio::AsyncResult>
class async_result : public task<RESULT, async_result_promise_type_t<RESULT>>
{
public:
  using super_type = task<RESULT, async_result_promise_type_t<RESULT>>;

  using super_type::super_type;

  using super_type::await_suspend;

  // NOTE; in general, if await_ready == false, then there is no result yet,
  // so the handle's frame should not have co_return'ed yet
  // so the handle/promise should still be valid

  template<typename OR>
  void await_suspend(
      std::coroutine_handle<async_result_promise_type_t<OR>> handle)
  {
    // propagate cancellable
    // the handle should be valid
    // (as await_ready == false, otherwise no suspend should happen)
    handle.promise().cancel_ = this->promise().cancel_;
    // usual suspend
    super_type::await_suspend(handle);
  }

  operator Gio::AsyncReadyCallback()
  {
    // only makes sense in default case
    static_assert(std::is_same<RESULT, Gio::AsyncResult>::value, "");
    if (this->p_) {
      // setup for new gio call
      this->result_ = this->p_->get_return_object(true).f;
      // also arrange for new cancellable below
      this->p_->cancel_ = nullptr;
      return [this](GObject_::Object, Gio::AsyncResult result) {
        this->promise().return_value(std::move(result));
      };
    } else {
      // so this task is the result of a coroutine frame
      // then it should be completed by the latter, not a Gio callback
      g_warning("no callback to complete coroutine");
      return nullptr;
    }
  }

  Gio::Cancellable cancellable()
  {
    Gio::Cancellable ret;
    if (this->p_) {
      if (!this->p_->cancel_)
        this->p_->cancel_ = Gio::Cancellable::new_();
      ret = this->p_->cancel_;
    } else if (!this->await_ready()) {
      // no create here, only propagate
      ret = this->promise().cancel_;
    }
    return ret;
  }

  static Gio::Cancellable timeout(
      const std::chrono::milliseconds &to, Gio::Cancellable cancel)
  {
    if (to.count() > 0 && cancel) {
      GLib::timeout_add_once(
          to.count(), [cancel]() mutable { cancel.cancel(); });
    }
    return cancel;
  }
};

async_result<void>
sleep_for(std::chrono::milliseconds to)
{
  async_result<void> w;
  GLib::timeout_add_once(to.count(), [&w] { w.promise().return_void(); });
  co_await w;
  co_return;
}

static task<void>
async_client(int port, int id, int &count)
{
  async_result w;

  auto dest = Gio::NetworkAddress::new_loopback(port);

  std::string sid = "client ";
  sid += std::to_string(id);

  // connect a client
  std::cout << sid << ": connect" << std::endl;
  auto client = Gio::SocketClient::new_();
  client.connect_async(dest, w);
  auto conn = gi::expect(client.connect_finish(co_await w));

  // say something
  auto os = conn.get_output_stream();
  std::cout << sid << ": send: " << sid << std::endl;
  os.write_all_async(
      (guint8 *)sid.data(), sid.size(), GLib::PRIORITY_DEFAULT_, w);
  os.write_all_finish(co_await w, (gsize *)nullptr);

  // now hear what the other side has to say
  std::cout << sid << ": receive" << std::endl;
  auto is = conn.get_input_stream();
  while (1) {
    guint8 data[1024];
    is.read_async(data, sizeof(data), GLib::PRIORITY_DEFAULT_, w);
    auto size = gi::expect(is.read_finish(co_await w));
    if (!size)
      break;
    std::string msg(data, data + size);
    std::cout << sid << ": got data: " << msg << std::endl;
  }

  std::cout << sid << ": closing down" << std::endl;
  --count;
}

static task<void>
async_handle_client(Gio::SocketConnection conn)
{
  async_result w;

  // say hello
  auto os = conn.get_output_stream();
  std::string msg = "hello ";
  os.write_all_async(
      (guint8 *)msg.data(), msg.size(), GLib::PRIORITY_DEFAULT_, w);
  os.write_all_finish(co_await w, (gsize *)nullptr);

  // now echo what the other side has to say
  auto is = conn.get_input_stream();
  while (1) {
    guint8 data[1024];
    // give up if timeout
    GLib::Error error;
    is.read_async(data, sizeof(data), GLib::PRIORITY_DEFAULT_,
        w.timeout(std::chrono::milliseconds(200), w.cancellable()), w);
    auto size = gi::expect(is.read_finish(co_await w, &error));
    if (error) {
      if (error.matches(G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        break;
      } else {
        gi::detail::try_throw(std::move(error));
      }
    }
    std::string msg(data, data + size);
    std::cout << "server: got data: " << msg << std::endl;
    os.write_all_async(data, size, GLib::PRIORITY_DEFAULT_, w);
    os.write_all_finish(co_await w, (gsize *)nullptr);
  }

  std::cout << "server: closing down client" << std::endl;
}

static task<void>
async_server(int clients, int &port)
{
  async_result w;

  auto listener = Gio::SocketListener::new_();
  port = gi::expect(listener.add_any_inet_port());

  int count = 0;
  while (count < clients) {
    // accept clients
    std::cout << "server: accepting" << std::endl;
    listener.accept_async(w);
    auto conn = gi::expect(
        listener.accept_finish(co_await w, (GObject_::Object *)nullptr));

    // spawn client handler
    std::cout << "server: new connection" << std::endl;
    // task will run itself to completion, no need to wait/watch it here
    async_handle_client(conn);
    ++count;
  }

  // wait a bit more and shutdown, because we can
  co_await sleep_for(std::chrono::milliseconds(1000));
}

void
async_demo(GLib::MainLoop loop, int clients)
{
  // run server
  // dispatch at once to obtain port
  int port = 0;
  auto server = async_server(clients, port);

  // make clients
  int count = 0;
  for (int i = 0; i < clients; ++i) {
    ++count;
    // client task runs to completion, no need to wait/watch
    // NOTE this frame will stay alive, so count ref is valid
    async_client(port, i, count);
  }

  // plain-and-simple; poll regularly and quit when all clients done
  task<void> cd;
  auto check = [&]() -> gboolean {
    if (!count) {
      cd.promise().return_void();
      return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
  };
  GLib::timeout_add(100, check);

  auto wait = [&]() -> task<void> {
    // wait clients
    co_await cd;
    // server should also have completed
    co_await server;
    std::cout << "server down" << std::endl;
    loop.quit();
  };

  wait();

  std::cout << "running loop" << std::endl;
  loop.run();
  std::cout << "ending loop" << std::endl;
}

int
main(int argc, char **argv)
{
  auto loop = GLib::MainLoop::new_();

  int clients = argc > 1 ? std::stoi(argv[1]) : 0;
  std::cout << clients << " clients" << std::endl;
  if (clients > 0)
    async_demo(loop, clients);
}
