#include "net/net.h"

#include <iostream>
#include <memory>
#include <string>
#include <functional>

class HttpClient {
public:
    HttpClient(net::io_context& ioc, tcp::endpoint base_endpoint)
        : _ioc(ioc)
        , _base_endpoint(base_endpoint) 
        {
        }

    ~HttpClient() {
        error_code shutdown_ec;
        _stream->socket().shutdown(tcp::socket::shutdown_both, shutdown_ec);
        if (shutdown_ec) {
            std::cerr << "shutdown: " << shutdown_ec.value() << "\n";
        }
    }

    net::awaitable<error_code> request(std::string const& target, std::string const& req_body, std::string& resp) {
        auto exec = co_await net::this_coro::executor;

        if (!_stream) {
            auto conn_ec = co_await do_connect();
            if (conn_ec) {
                co_return conn_ec;
            }
        }

        auto req_ec = co_await do_request(target, req_body, resp);
        if (req_ec) {
            co_return req_ec;
        }
        
        co_return error_code{};
    }


private:
    net::awaitable<error_code> do_connect() {
        auto exec = co_await net::this_coro::executor;
        _stream = std::make_unique<beast::tcp_stream>(exec);
        _stream->expires_after(std::chrono::seconds(30));
        auto [connect_ec] = co_await _stream->async_connect(_base_endpoint, use_nothrow_awaitable);
        if(connect_ec) {
            std::cerr << "connect: " << connect_ec.message() << "\n";

            co_return connect_ec;
        }

        co_return error_code{};
    }

    net::awaitable<error_code> do_request(std::string const& target, std::string const& req_body, std::string& resp) {
        int retry_times = 0;
        int max_retry_times = 2;

        while (true) {
            http::request<http::string_body> req{http::verb::post, target, 11};
            req.set(http::field::host, _base_endpoint.address().to_string());
            req.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
            req.keep_alive(true);
            req.body() = req_body;
            req.prepare_payload();

            _stream->expires_after(std::chrono::seconds(30));
            auto [write_ec, write_size] = co_await http::async_write(*_stream, req, use_nothrow_awaitable);
            if (write_ec) {
                std::cerr << "write - [" << write_ec.value() << "]: " << write_ec.message() << "\n";

                ++retry_times;
                if (retry_times == max_retry_times) {
                    co_return write_ec;
                }

                co_await do_connect();
                continue;
            } 

            http::response<http::dynamic_body> res;
            auto [read_ec, read_size] = co_await http::async_read(*_stream, _buffer, res, use_nothrow_awaitable);
            if(read_ec) {
                std::cerr << "read - [" << read_ec.value() << "]: " << read_ec.message() << "\n";

                ++retry_times;
                if (retry_times == max_retry_times) {
                    co_return read_ec;
                }

                co_await do_connect();
                continue;
            }

            resp = beast::buffers_to_string(res.body().data());
            _buffer.consume(_buffer.size());

            co_return error_code{};
        }
    }

private:
    net::io_context& _ioc;
    tcp::endpoint _base_endpoint;
    std::unique_ptr<beast::tcp_stream> _stream;
    beast::flat_buffer _buffer;
};

int main(int argc, char* argv[]) {
    try {
        if (argc != 3) {
            std::cerr << "Usage: \n";
            std::cerr << " <target_address> <target_port>\n";
            return 1;
        }

        net::io_context ctx;

        auto endpoint = *tcp::resolver(ctx).resolve(argv[1], argv[2]);

        HttpClient client(ctx, endpoint);
        std::string target = "/";
        std::string req = "abcdef";
        std::string resp;

        net::co_spawn(
            ctx,
            std::bind(
                &HttpClient::request,
                &client,
                std::cref(target),
                std::cref(req),
                std::ref(resp)), 
            net::detached);

        ctx.run();

        std::cout << resp << std::endl;
    }
    catch (std::exception &e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    return 0;
}