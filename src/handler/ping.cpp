#include "cocaine/proxy/handler/ping.hpp"

using namespace cocaine::proxy;
using namespace cocaine::proxy::handler;

void
ping_t::on_request(const thevoid::http_request&, const boost::asio::const_buffer&) {
    send_reply(thevoid::http_response::ok);
}
