#include "net/net.h"

#include <iostream>

net::awaitable<error_code> do_connect(tcp::endpoint& endpoint, beast::tcp_stream& stream) {
    stream.expires_after(std::chrono::seconds(30));
    auto [connect_ec] = co_await stream.async_connect(endpoint, use_nothrow_awaitable);
    if(connect_ec) {
        std::cerr << "connect: " << connect_ec.message() << "\n";

        co_return connect_ec;
    }

    error_code ec;
    co_return ec;
}

net::awaitable<void> do_session(tcp::endpoint endpoint) {
    auto exec = co_await net::this_coro::executor;
    auto stream_ptr = std::make_unique<beast::tcp_stream>(exec);
    beast::flat_buffer buffer;

    co_await do_connect(endpoint, *stream_ptr);

    int retry_times = 0;

    while (true) {
begin:
        http::request<http::string_body> req{http::verb::get, "/", 11};
        req.set(http::field::host, endpoint.address().to_string());
        req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
        req.keep_alive(true);
        req.body() = "abcd";
        req.prepare_payload();

        stream_ptr->expires_after(std::chrono::seconds(30));
        auto [write_ec, write_size] = co_await http::async_write(*stream_ptr, req, use_nothrow_awaitable);
        if (write_ec) {
            std::cerr << "write - [" << write_ec.value() << "]: " << write_ec.message() << "\n";

            if (retry_times < 2) {
                ++retry_times;
                stream_ptr = std::make_unique<beast::tcp_stream>(exec);
				co_await do_connect(endpoint, *stream_ptr);
                goto begin;
            }

            co_return;
        } 

        http::response<http::dynamic_body> res;
        auto [read_ec, read_size] = co_await http::async_read(*stream_ptr, buffer, res, use_nothrow_awaitable);
        if(read_ec) {
            std::cerr << "read - [" << read_ec.value() << "]: " << read_ec.message() << "\n";
            if (retry_times < 2) {
                ++retry_times;
                stream_ptr = std::make_unique<beast::tcp_stream>(exec);
				co_await do_connect(endpoint, *stream_ptr);
                goto begin;
            }

            co_return;
        }

        retry_times = 0;
        std::cout << res << std::endl;
        buffer.consume(buffer.size());

        net::steady_timer timer(exec);
        timer.expires_after(std::chrono::seconds(2));
        co_await timer.async_wait(use_nothrow_awaitable);
    }

    error_code shutdown_ec;
    stream_ptr->socket().shutdown(tcp::socket::shutdown_both, shutdown_ec);
    if (shutdown_ec) {
        std::cerr << "shutdown: " << shutdown_ec.value() << "\n";
    }
}

int main(int argc, char* argv[]) {
    try {
        if (argc != 3) {
            std::cerr << "Usage: \n";
            std::cerr << " <target_address> <target_port>\n";
            return 1;
        }

        net::io_context ctx;

        auto endpoint = *tcp::resolver(ctx).resolve(argv[1], argv[2]);

        net::co_spawn(ctx, do_session(endpoint), net::detached);

        ctx.run();
    }
    catch (std::exception &e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    return 0;
}