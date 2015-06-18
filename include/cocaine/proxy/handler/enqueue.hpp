#pragma once

#include <thevoid/stream.hpp>

#include <cocaine/framework/future.hpp>

#include "cocaine/proxy/common.hpp"

namespace cocaine { namespace framework {
    class app_service_t;
}}

namespace cocaine { namespace proxy {
    class server_t;

    template<class T> class service_wrapper;
}}

namespace cocaine { namespace proxy { namespace handler {

class tracing_t {
public:
    typedef std::chrono::system_clock clock_type;
    typedef clock_type::time_point    time_point;

    struct dump_t {
        /// Immediately after creation.
        time_point birth;

        /// Immediately after accepting the request.
        time_point accepted;

        /// Immediately either after obtaining connected service or on connection error.
        time_point loaded;

        /// Immediately after obtaining headers.
        time_point fresh;

        /// Immediately after obtaining first body chunk.
        time_point streaming;

        /// Immediately after obtaining the last body chunk (and being closed).
        time_point closed;
    };

private:
    dump_t data;

public:
    tracing_t()
    {
        bind(&data.birth);
        data.streaming = clock_type::from_time_t(0);
        data.closed    = clock_type::from_time_t(0);
    }

    void
    accepted() {
        bind(&data.accepted);
    }

    void
    loaded() {
        bind(&data.loaded);
    }

    void
    headers() {
        bind(&data.fresh);
    }

    void
    body() {
        if (data.streaming.time_since_epoch().count() == 0) {
            bind(&data.streaming);
            bind(&data.closed);
        } else {
            bind(&data.closed);
        }
    }

    dump_t
    dump() const {
        return data;
    }

private:
    void
    bind(time_point* tp) {
        *tp = clock_type::now();
    }
};

class enqueue_t:
    public thevoid::simple_request_stream<server_t>,
    public std::enable_shared_from_this<enqueue_t>
{
    struct route_t;
    class response_stream_t;

    typedef framework::app_service_t service_type;
    typedef framework::future<service_wrapper<service_type>> service_future_type;

    tracing_t checkpoint;

public:
    enqueue_t();

    virtual
    void
    on_request(const thevoid::http_request& rq, const boost::asio::const_buffer& buf);

private:
    void
    process(const route_t& route, const thevoid::http_request& rq, const boost::asio::const_buffer& buf);

    void
    on_service(service_future_type& future, route_t route, thevoid::http_request rq, std::string body);

    static
    boost::optional<route_t>
    extract_routing(const ioremap::thevoid::http_request &rq);
};

}}} // namespace cocaine::proxy::handler
