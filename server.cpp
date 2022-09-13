#include "net/net.h"

#include <iostream>
#include <memory>
#include <deque>
#include <string>
#include <list>

class Server;

class HttpClient {
public:
    HttpClient() = default;

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
    tcp::endpoint _base_endpoint;
    std::unique_ptr<beast::tcp_stream> _stream;
    beast::flat_buffer _buffer;
};

class WebsocketClient {
public:
    using ReceiveMessageCallbackType = std::function<net::awaitable<void>(std::string)>;

    WebsocketClient(net::io_context &ioc, 
        tcp::endpoint const &endpoint, 
        std::string const &target)
        : _reconnect_times(0)
        , _base_endpoint(endpoint)
        , _target(target)
        , _connect_ready_signal(ioc)
    {
        _connect_ready_signal.expires_at(std::chrono::steady_clock::time_point::max());
        _receive_message_callback = default_receive_message_callback;
    }

    void set_receive_message_callback(ReceiveMessageCallbackType func) {
        _receive_message_callback = func;
    }

    void connect(net::io_context &ioc)
    {
        net::co_spawn(
            ioc,
            do_connect(),
            net::detached);
    }

    net::awaitable<error_code> request(std::string req_body)
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
    static net::awaitable<void> default_receive_message_callback(std::string const& ) { co_return; }

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

            co_await _receive_message_callback(std::move(msg));
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
    std::deque<std::shared_ptr<std::string>> _req_queue;

    ReceiveMessageCallbackType _receive_message_callback;
};

class ConnectionSession
{
public:
    friend class Server;

    explicit ConnectionSession(tcp::socket socket)
        : _ws(std::move(socket))
    {
        _ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
        _ws.set_option(websocket::stream_base::decorator(
            [](websocket::response_type &res)
            { res.set(http::field::server, std::string(BOOST_BEAST_VERSION_STRING) + " websocket-server-async"); }));
    }

    net::awaitable<void> send(std::shared_ptr<std::string> msg)
    {
        _msg_queue.emplace_back(std::move(msg));
        if (_msg_queue.size() != 1)
        {
            co_return;
        }
        while (not _msg_queue.empty())
        {
            auto [ec, write_size] = co_await _ws.async_write(net::buffer(*_msg_queue.front()), use_nothrow_awaitable);
            if (ec)
            {
                std::cerr << "write: " << ec.message() << "\n";

                co_return;
            }
            _msg_queue.pop_front();
        }
    }

private:
    websocket::stream<beast::tcp_stream> _ws;
    std::deque<std::shared_ptr<std::string const>> _msg_queue;
    HttpClient _http_client;
};

class Server
{
public:
    friend class WebsocketClient;

    Server(net::io_context &ioc, tcp::endpoint const &bind_endpoint)
        : _bind_endpoint(bind_endpoint)
        , _acceptor(ioc)
    {
    }

    void run(
        net::io_context &ioc,
        tcp::endpoint const &http_target_base_endpoint,
        tcp::endpoint const &websocket_target_base_endpoint,
        std::string const &websocket_target_url)
    {
        _http_target_base_endpoint = http_target_base_endpoint;
        _websocket_target_base_endpoint = websocket_target_base_endpoint;
        _websocket_target_url = websocket_target_url;

        _websocket_client = std::make_unique<WebsocketClient>(
            ioc, _websocket_target_base_endpoint, _websocket_target_url);
        _websocket_client->set_receive_message_callback(std::bind_front(&Server::handle_websocket_receive_message, this));
        _websocket_client->connect(ioc);

        error_code ec;
        _acceptor.open(_bind_endpoint.protocol(), ec);
        if (ec)
        {
            std::cerr << "open: " << ec.message() << "\n";
            return;
        }
        _acceptor.set_option(net::socket_base::reuse_address(true), ec);
        if (ec)
        {
            std::cerr << "set_option: " << ec.message() << "\n";
            return;
        }
        _acceptor.bind(_bind_endpoint, ec);
        if (ec)
        {
            std::cerr << "bind: " << ec.message() << "\n";
            return;
        }
        _acceptor.listen(net::socket_base::max_listen_connections, ec);
        if (ec)
        {
            std::cerr << "listen: " << ec.message() << "\n";
            return;
        }
        net::co_spawn(ioc, do_accept(), net::detached);
    }

private:
    net::awaitable<void> do_accept()
    {
        while (true)
        {
            auto [ec, socket] = co_await _acceptor.async_accept(use_nothrow_awaitable);
            if (ec)
            {
                std::cerr << "listen: " << ec.message() << "\n";
                break;
            }
            auto exec = socket.get_executor();
            net::co_spawn(exec, do_session(std::move(socket)), net::detached);
        }
    }

    net::awaitable<void> broadcast_response(std::shared_ptr<std::string> resp)
    {
        _response_queue.emplace_back(std::move(resp));
        if (_response_queue.size() != 1)
        {
            co_return;
        }
        while (not _response_queue.empty())
        {
            for (auto &conn : _connections)
            {
                co_await conn->send(_response_queue.front());
            }
            _response_queue.pop_front();
        }
    }

    net::awaitable<void> handle_websocket_receive_message(std::string msg)
    {
        auto exec = co_await net::this_coro::executor;
        auto msg_ptr = std::make_shared<std::string>(std::move(msg));
        net::co_spawn(exec, broadcast_response(msg_ptr), net::detached);
    }

    net::awaitable<void> handle_http_req(std::shared_ptr<ConnectionSession> session, std::string req_body)
    {
        std::string target = "/";
        auto result_msg = std::make_shared<std::string>();
        auto ec = co_await session->_http_client.request(target, req_body, *result_msg);

        auto exec = co_await net::this_coro::executor;
        co_await broadcast_response(result_msg);
    }

    net::awaitable<void> do_session(tcp::socket socket)
    {
        auto session = std::make_shared<ConnectionSession>(std::move(socket));
        session->_http_client.set_target_base_endpoint(_http_target_base_endpoint);
        beast::flat_buffer buffer;

        auto [accept_ec] = co_await session->_ws.async_accept(use_nothrow_awaitable);
        if (accept_ec)
        {
            std::cerr << "accept: " << accept_ec.message() << "\n";

            co_return;
        }
        _connections.emplace_back(session);
        auto iter = std::next(_connections.end(), -1);

        while (true)
        {
            auto [read_ec, read_size] = co_await session->_ws.async_read(buffer, use_nothrow_awaitable);
            if (read_ec == websocket::error::closed)
            {
                break;
            }
            if (read_ec)
            {
                std::cerr << "read: " << read_ec.message() << "\n";
                break;
            }

            auto msg = beast::buffers_to_string(buffer.data());
            buffer.consume(buffer.size());

            // TODO: dispatch using msg content
            // 按照 http request 处理
            auto exec = socket.get_executor();
            net::co_spawn(exec, handle_http_req(session, msg), net::detached);
            // 按照 websocket 处理
            net::co_spawn(exec, _websocket_client->request(std::move(msg)), net::detached);
        }

        _connections.erase(iter);
        auto [close_ec] = co_await session->_ws.async_close(websocket::close_code::normal, use_nothrow_awaitable);
        if (close_ec)
        {
            std::cerr << "close: " << close_ec.message() << "\n";
        }
    }

private:
    tcp::endpoint _bind_endpoint; // ws server binding endpoint
    tcp::acceptor _acceptor;
    std::list<std::shared_ptr<ConnectionSession>> _connections;
    std::unique_ptr<WebsocketClient> _websocket_client;
    tcp::endpoint _http_target_base_endpoint;      // http target base endpoint
    tcp::endpoint _websocket_target_base_endpoint; // websocket target base endpoint
    std::string _websocket_target_url;             // websocket target url

    std::deque<std::shared_ptr<std::string>> _response_queue;
};

int main(int argc, char *argv[])
{
    try
    {
        if (argc != 8)
        {
            std::cerr << "Usage: \n";
            std::cerr << " <listen_address> <listen_port> <http_target_address> <http_target_port> <websocket_target_address> <websocket_target_prt> <websocket_target_url>\n";
            return 1;
        }

        net::io_context ioc;
        auto endpoint = *tcp::resolver(ioc).resolve(argv[1], argv[2], tcp::resolver::passive);
        auto http_target_endpoint = *tcp::resolver(ioc).resolve(argv[3], argv[4]);
        auto websocket_target_endpoint = *tcp::resolver(ioc).resolve(argv[5], argv[6]);

        Server server(ioc, endpoint);
        server.run(ioc, http_target_endpoint, websocket_target_endpoint, argv[7]);

        ioc.run();
    }
    catch (std::exception &e)
    {
        std::cerr << "Exception: " << e.what() << "\n";
    }

    return 0;
}