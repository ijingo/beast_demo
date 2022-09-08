#include "net/net.h"

#include <iostream>
#include <memory>
#include <deque>
#include <string>
#include <unordered_set>


class Server;

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
    {
    }

	void run() {
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
			auto msg_ptr = std::make_shared<std::string>(std::move(msg));
			for (auto& conn : _connections) {
				co_await conn->send(msg_ptr);
			}
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
};

int main(int argc, char* argv[]) {
    try {
        if (argc != 3) {
            std::cerr << "Usage: \n";
            std::cerr << " <listen_address> <listen_port>\n";
            return 1;
        }

        net::io_context ioc;
		auto endpoint = *tcp::resolver(ioc).resolve(argv[1], argv[2], tcp::resolver::passive);

        Server server(ioc, endpoint);
        server.run();

        ioc.run();
    }
    catch (std::exception &e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    return 0;
}