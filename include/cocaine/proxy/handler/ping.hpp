#pragma once

#include <thevoid/stream.hpp>

#include "cocaine/proxy/common.hpp"

namespace cocaine { namespace proxy {
    class server_t;
}}

namespace cocaine { namespace proxy { namespace handler {

struct ping_t:
    public thevoid::simple_request_stream<server_t>
{
    virtual
    void
    on_request(const thevoid::http_request& req, const boost::asio::const_buffer &buffer);
};

}}} // namespace cocaine::proxy::handler
