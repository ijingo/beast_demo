#include "net/net.h"

#include <iostream>
#include <memory>

net::awaitable<error_code> do_connect(tcp::endpoint& endpoint, websocket::stream<beast::tcp_stream>& ws) {
    auto& socket = beast::get_lowest_layer(ws);
    socket.expires_after(std::chrono::seconds(30));
    auto [connect_ec] = co_await socket.async_connect(endpoint, use_nothrow_awaitable);
    if(connect_ec) {
        std::cerr << "connect: " << connect_ec.message() << "\n";

        co_return connect_ec;
    }
    socket.expires_never();
     ws.set_option(
        websocket::stream_base::timeout::suggested(
            beast::role_type::client));

    ws.set_option(websocket::stream_base::decorator(
        [](websocket::request_type& req)
        {
            req.set(http::field::user_agent,
                std::string(BOOST_BEAST_VERSION_STRING) +
                    " websocket-client-coro");
        }));


    auto [handshake_ec] = co_await ws.async_handshake(endpoint.address().to_string(), "/", use_nothrow_awaitable);
    if (handshake_ec) {
        std::cerr << "handshake: " << handshake_ec.message() << "\n";

        co_return handshake_ec;
    }

    co_return error_code{};
}

net::awaitable<void> do_read(websocket::stream<beast::tcp_stream>& ws) {
    beast::flat_buffer buffer;
    while(true) {
        auto [read_ec, read_size] = co_await ws.async_read(buffer, use_nothrow_awaitable);

        if (read_ec) {
            std::cerr << "read: " << read_ec.message() << "\n";
            break;
        }

        auto msg = beast::buffers_to_string(buffer.data());
        buffer.consume(buffer.size());

        std::cout << "received msg: " << msg << std::endl;
    }
}

net::awaitable<void> do_write(websocket::stream<beast::tcp_stream>& ws) {
    auto exec = ws.get_executor();

    std::string str = "abcdef";

    while(true) {
        auto [ec, write_size] = co_await ws.async_write(net::buffer(str), use_nothrow_awaitable);
        if (ec) {
            std::cerr << "write: " << ec.message() << "\n";

            break;
        }

        net::steady_timer timer(exec);
        timer.expires_after(std::chrono::seconds(2));
        co_await timer.async_wait(use_nothrow_awaitable);
    }
}


net::awaitable<void> do_session(tcp::endpoint endpoint, std::string& resp) {
    auto exec = co_await net::this_coro::executor;

    websocket::stream<beast::tcp_stream> ws(exec);
    auto conn_ec = co_await do_connect(endpoint, ws);
    if (conn_ec) {
        co_return;
    }

    net::co_spawn(exec, do_read(ws), net::detached);
    net::co_spawn(exec, do_write(ws), net::detached);
}


int main(int argc, char* argv[]) {
    try {
        if (argc != 3) {
            std::cerr << "Usage: \n";
            std::cerr << " <listen_address> <listen_port>\n";
            return 1;
        }

        net::io_context ctx;

        auto endpoint = *tcp::resolver(ctx).resolve(argv[1], argv[2]);

        std::string resp;
        net::co_spawn(ctx, do_session(endpoint, std::ref(resp)), net::detached);

        ctx.run();
    }
    catch (std::exception &e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    return 0;
}