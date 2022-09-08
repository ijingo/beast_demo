#ifndef NET_NET_H
#define NET_NET_H

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/asio/experimental/as_tuple.hpp>

namespace net = boost::asio;                    // namespace asio
using tcp = net::ip::tcp;                       // from <boost/asio/ip/tcp.hpp>

namespace beast = boost::beast;
namespace http = boost::beast::http;            // from <boost/beast/http.hpp>
namespace websocket = boost::beast::websocket;  // from <boost/beast/websocket.hpp>

using error_code = boost::system::error_code;      // from <boost/system/error_code.hpp>
using system_error = boost::system::system_error;  // from <boost/system/system_error.hpp>

constexpr auto use_nothrow_awaitable = boost::asio::experimental::as_tuple(net::use_awaitable);

#endif // NET_NET_H
