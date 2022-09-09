#include "net/net.h"

#include <iostream>
#include <memory>
#include <string>
#include <functional>
#include <deque>

class WebsocketClient {
public:
    WebsocketClient(net::io_context &ioc, 
        tcp::endpoint const &endpoint, 
        std::string const &target)
        : _reconnect_times(0)
        , _base_endpoint(endpoint)
        , _target(target)
        , _connect_ready_signal(ioc)
    {
        _connect_ready_signal.expires_at(std::chrono::steady_clock::time_point::max());
    }

    void connect(net::io_context &ioc)
    {
        net::co_spawn(
            ioc,
            do_connect(),
            net::detached);
    }

    net::awaitable<error_code> request(std::string const &req_body)
    {
        co_await _connect_ready_signal.async_wait(use_nothrow_awaitable);
        auto ws = _ws;
        _req_queue.emplace_back(std::make_shared<std::string>(req_body));
        if (_req_queue.size() != 1)
        {
            co_return error_code{};
        }
        while (not _req_queue.empty())
        {
            auto [ec, write_size] = co_await ws->async_write(net::buffer(*_req_queue.front()), use_nothrow_awaitable);
            if (ec)
            {
                std::cerr << "write: " << ec.message() << "\n";

                co_return ec;
            }
            _req_queue.pop_front();
        }

        co_return error_code{};
    }

private:
    net::awaitable<void> handle_read(std::weak_ptr<websocket::stream<beast::tcp_stream>> ws_weak_ptr)
    {
        beast::flat_buffer buffer;
        while (auto ws = ws_weak_ptr.lock())
        {

            auto [read_ec, read_size] = co_await ws->async_read(buffer, use_nothrow_awaitable);

            if (read_ec)
            {
                std::cerr << "read: " << read_ec.message() << "\n";

                ++_reconnect_times;
                co_await do_connect();

                break;
            }

            auto msg = beast::buffers_to_string(buffer.data());
            buffer.consume(buffer.size());

            // TODO(wj): call handle msg here
            std::cout << "received msg: " << msg << std::endl;
        }
    }

    net::awaitable<void> do_connect()
    {
        int max_reconnect_times = 3;
        if (_reconnect_times == max_reconnect_times)
        {
            std::cerr << "max reconnect times for websocket client!\n";
            co_return;
        }

        _connect_ready_signal.expires_at(std::chrono::steady_clock::time_point::max());
        auto exec = co_await net::this_coro::executor;
        _ws = std::make_shared<websocket::stream<beast::tcp_stream>>(exec);

        auto &socket = beast::get_lowest_layer(*_ws);
        socket.expires_after(std::chrono::seconds(30));
        auto [connect_ec] = co_await socket.async_connect(_base_endpoint, use_nothrow_awaitable);
        if (connect_ec)
        {
            std::cerr << "connect: " << connect_ec.message() << "\n";

            co_return;
        }

        socket.expires_never();
        _ws->set_option(
            websocket::stream_base::timeout::suggested(
                beast::role_type::client));

        _ws->set_option(websocket::stream_base::decorator(
            [](websocket::request_type &req)
            {
                req.set(http::field::user_agent,
                        std::string(BOOST_BEAST_VERSION_STRING) +
                            " websocket-client-coro");
            }));

        auto [handshake_ec] = co_await _ws->async_handshake(
            _base_endpoint.address().to_string(), _target, use_nothrow_awaitable);
        if (handshake_ec)
        {
            std::cerr << "handshake: " << handshake_ec.message() << "\n";
        }

        _connect_ready_signal.expires_at(std::chrono::steady_clock::time_point::min());
        net::co_spawn(exec, handle_read(_ws), net::detached);
    }

private:
    int _reconnect_times;
    tcp::endpoint _base_endpoint;
    std::string _target;
    net::steady_timer _connect_ready_signal;
    std::shared_ptr<websocket::stream<beast::tcp_stream>> _ws;
    std::deque<std::shared_ptr<std::string const>> _req_queue;
};

net::awaitable<void> do_request(WebsocketClient& client) {
    auto exec = co_await net::this_coro::executor;

    while (true) {
        auto ec = co_await client.request("writing msgs");
        if (ec) {
            std::cerr << "request: " << ec.message() << "\n";
        }

        net::steady_timer timer(exec);
        timer.expires_after(std::chrono::seconds(2));
        co_await timer.async_wait(use_nothrow_awaitable);
    }
}

int main(int argc, char *argv[])
{
    try
    {
        if (argc != 3)
        {
            std::cerr << "Usage: \n";
            std::cerr << " <http_target_address> <http_target_port>\n";
            return 1;
        }

        net::io_context ctx;

        auto endpoint = *tcp::resolver(ctx).resolve(argv[1], argv[2]);

        WebsocketClient client(ctx, endpoint, "/");
        client.connect(ctx);
        net::co_spawn(ctx, do_request(client), net::detached);

        ctx.run();
    }
    catch (std::exception &e)
    {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    return 0;
}