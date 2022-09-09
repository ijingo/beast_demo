#include "net/net.h"

#include <iostream>
#include <memory>

net::awaitable<void> echo(tcp::socket socket) {
    websocket::stream<beast::tcp_stream> ws{std::move(socket)};
    ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    ws.set_option(websocket::stream_base::decorator(
        [](websocket::response_type &res) { res.set(http::field::server, std::string(BOOST_BEAST_VERSION_STRING) + " websocket-server-async"); }));

    beast::flat_buffer buffer;

	auto [accept_ec] = co_await ws.async_accept(use_nothrow_awaitable);
    if (accept_ec) {
        std::cerr << "accept: " << accept_ec.message() << "\n";

        co_return;
    }

    while (true) {
        auto [read_ec, read_size] = co_await ws.async_read(buffer, use_nothrow_awaitable);
        if(read_ec == websocket::error::closed) {
            break;
        }
        if (read_ec) {
            std::cerr << "read: " << read_ec.message() << "\n";
            break;
        }

        auto msg = beast::buffers_to_string(buffer.data());
        buffer.consume(buffer.size());

        std::cout << "got one message: " << msg << std::endl;

        auto [write_ec, write_size] = co_await ws.async_write(net::buffer(msg), use_nothrow_awaitable);
        if (write_ec) {
            std::cerr << "write: " << write_ec.message() << "\n";
            break;
        }
    }

    auto [close_ec] = co_await ws.async_close(websocket::close_code::normal, use_nothrow_awaitable);
    if (close_ec) {
        std::cerr << "close: " << close_ec.message() << "\n";
    }
}

net::awaitable<void> listen(tcp::acceptor& acceptor) {
    while (true) {
        auto [ec, socket] = co_await acceptor.async_accept(use_nothrow_awaitable);
        if (ec) {
            std::cerr << "listen: " << ec.message() << "\n";
            break;
        }
        auto exec = socket.get_executor();
        net::co_spawn(exec, echo(std::move(socket)), net::detached);
    }
}

int main(int argc, char* argv[]) {
    try {
        if (argc != 3) {
            std::cerr << "Usage: \n";
            std::cerr << " <listen_address> <listen_port>\n";
            return 1;
        }

        net::io_context ctx;

        auto listen_endpoint =
            *tcp::resolver(ctx).resolve(argv[1], argv[2], tcp::resolver::passive);

        tcp::acceptor acceptor(ctx, listen_endpoint);

        net::co_spawn(ctx, listen(acceptor), net::detached);

        ctx.run();
    }
    catch (std::exception &e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    return 0;
}