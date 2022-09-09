#include "net/net.h"

#include <iostream>
#include <memory>
#include <deque>
#include <string>
#include <unordered_set>

class Server;

class HttpClient {
public:
    HttpClient(net::io_context& ioc)
        : _ioc(ioc)
        {
        }

    ~HttpClient() {
        error_code shutdown_ec;
        _stream->socket().shutdown(tcp::socket::shutdown_both, shutdown_ec);
        if (shutdown_ec) {
            std::cerr << "shutdown: " << shutdown_ec.value() << "\n";
        }
    }

    void set_target_base_endpoint(tcp::endpoint const& endpoint) {
        _base_endpoint = endpoint;
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


class ConnectionSession {
public:
    friend Server;

    ConnectionSession(tcp::socket socket)
        : _ws(std::move(socket))
    {
		_ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
		_ws.set_option(websocket::stream_base::decorator(
			[](websocket::response_type &res) { res.set(http::field::server, std::string(BOOST_BEAST_VERSION_STRING) + " websocket-server-async"); }));
    }

    net::awaitable<void> send(std::shared_ptr<std::string> msg) {
        _msg_queue.emplace_back(std::move(msg));
        if (_msg_queue.size() != 1) {
            co_return;
        }
        while (not _msg_queue.empty()) {
            auto [ec, write_size] = co_await _ws.async_write(net::buffer(*_msg_queue.front()), use_nothrow_awaitable);
            if (ec) {
                std::cerr << "write: " << ec.message() << "\n";

                co_return;
            }
            _msg_queue.pop_front();
        }
    }
private:
    websocket::stream<beast::tcp_stream> _ws;
    std::deque<std::shared_ptr<std::string const>> _msg_queue;
};

class Server {
public:
    Server(net::io_context& ioc, tcp::endpoint endpoint)
        : _ioc(ioc)
        , _endpoint(endpoint)
        , _acceptor(ioc)
        , _http_client(ioc)
    {
    }

	void run(tcp::endpoint target_base_endpoint) {
        _http_client.set_target_base_endpoint(target_base_endpoint);

        error_code ec;
        _acceptor.open(_endpoint.protocol(), ec);
		if (ec) {
			std::cerr << "open: " << ec.message() << "\n";
            return;
        }
        _acceptor.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) {
			std::cerr << "set_option: " << ec.message() << "\n";
            return;
        }
        _acceptor.bind(_endpoint, ec);
        if (ec) {
			std::cerr << "bind: " << ec.message() << "\n";
            return;
        }
        _acceptor.listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
			std::cerr << "listen: " << ec.message() << "\n";
            return;
        }
		net::co_spawn(_ioc, do_accept(), net::detached);
	}

private: 
    net::awaitable<void> do_accept() {
        while (true) {
            auto [ec, socket] = co_await _acceptor.async_accept(use_nothrow_awaitable);
            if (ec) {
                std::cerr << "listen: " << ec.message() << "\n";
                break;
            }
            auto exec = socket.get_executor();
            net::co_spawn(exec, do_session(std::move(socket)), net::detached);
        }
    }

    net::awaitable<void> handle_message(std::string req_body) {
        std::string target ="/";
        auto result_msg = std::make_shared<std::string>();
        auto ec = co_await _http_client.request(target, req_body, *result_msg);

        for (auto& conn : _connections) {
            co_await conn->send(result_msg);
        }
    }

    net::awaitable<void> do_session(tcp::socket socket) {
		auto session = std::make_shared<ConnectionSession>(std::move(socket));
		beast::flat_buffer buffer;

		auto [accept_ec] = co_await session->_ws.async_accept(use_nothrow_awaitable);
		if (accept_ec) {
			std::cerr << "accept: " << accept_ec.message() << "\n";

			co_return;
		}
		_connections.emplace(session);

		while (true) {
			auto [read_ec, read_size] = co_await session->_ws.async_read(buffer, use_nothrow_awaitable);
			if(read_ec == websocket::error::closed) {
				break;
			}
			if (read_ec) {
				std::cerr << "read: " << read_ec.message() << "\n";
				break;
			}

			auto msg = beast::buffers_to_string(buffer.data());
			buffer.consume(buffer.size());
            net::co_spawn(_ioc, handle_message(std::move(msg)), net::detached) ;
		}

		_connections.erase(session);
		auto [close_ec] = co_await session->_ws.async_close(websocket::close_code::normal, use_nothrow_awaitable);
		if (close_ec) {
			std::cerr << "close: " << close_ec.message() << "\n";
		}
    }
    

private: 
    net::io_context& _ioc;
    tcp::endpoint _endpoint;
    tcp::acceptor _acceptor;
    std::unordered_set<std::shared_ptr<ConnectionSession>> _connections;

    HttpClient _http_client;
};

int main(int argc, char* argv[]) {
    try {
        if (argc != 5) {
            std::cerr << "Usage: \n";
            std::cerr << " <listen_address> <listen_port> <http_target_address> <http_target_port>\n";
            return 1;
        }

        net::io_context ioc;
		auto endpoint = *tcp::resolver(ioc).resolve(argv[1], argv[2], tcp::resolver::passive);
		auto http_target_endpoint = *tcp::resolver(ioc).resolve(argv[3], argv[4]);

        Server server(ioc, endpoint);
        server.run(http_target_endpoint);

        ioc.run();
    }
    catch (std::exception &e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    return 0;
}