#include "net/net.h"

#include <iostream>

http::response<http::string_body> handle_request(http::request<http::string_body>&& req) {
    http::response<http::string_body> resp;
    resp.version(req.version());
    resp.result(http::status::ok);
    resp.set(http::field::content_type, "text/plain");
    resp.keep_alive(req.keep_alive());
    resp.body() = req.body();
    resp.prepare_payload();

    return resp;
}

net::awaitable<void> echo(tcp::socket socket) {
    beast::flat_buffer buffer;
    beast::tcp_stream stream(std::move(socket));
    beast::error_code return_ec;

    while (true) {
		http::request<http::string_body> req;
        auto [read_ec, read_size] = co_await http::async_read(stream, buffer, req, use_nothrow_awaitable);
        if (read_ec == http::error::end_of_stream) {
            return_ec = read_ec;
            break;
        }
        if (read_ec) {
            std::cerr << "read: " << read_ec.message() << "\n";
            return_ec = read_ec;
            break;
        }

        std::cout << "got one request: " << req.body() << std::endl;

        auto resp = handle_request(std::move(req));
        bool keep_alive = resp.keep_alive();

        auto [write_ec, write_size] = co_await http::async_write(stream, std::move(resp), use_nothrow_awaitable);
        buffer.consume(buffer.size());
        if (write_ec) {
            std::cerr << "write: " << write_ec.message() << "\n";
            return_ec = write_ec;
            break;
        }
        if (!keep_alive) {
            break;
        }
    }
    
    stream.socket().shutdown(tcp::socket::shutdown_send, return_ec);
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