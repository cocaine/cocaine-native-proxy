#ifndef COCAINE_NATIVE_PROXY_HPP
#define COCAINE_NATIVE_PROXY_HPP

#include "service_pool.hpp"
#include "buffered_stream.hpp"
#include <cocaine/framework/services/app.hpp>
#include <thevoid/server.hpp>

namespace cocaine { namespace proxy {

class proxy :
    public ioremap::thevoid::server<proxy>
{
public:
    struct on_enqueue :
        public ioremap::thevoid::simple_request_stream<proxy>,
        public std::enable_shared_from_this<on_enqueue>
    {
        virtual
        void
        on_request(const ioremap::swarm::network_request &req,
                   const boost::asio::const_buffer &buffer);

        void
        on_resp_headers(cocaine::framework::generator<std::string>&);

        void
        on_resp_chunk(cocaine::framework::future<std::string>&);

        void
        on_resp_close(cocaine::framework::future<std::vector<cocaine::framework::future<void>>>&);

    private:
        std::shared_ptr<buffered_stream_t> m_buffered;
        bool m_chunked;
        size_t m_content_length;

        std::string m_application;
        std::string m_event;
    };
    friend struct on_enqueue;

public:
    bool
    initialize(const rapidjson::Value &config);

    ~proxy();

private:
    typedef std::map<std::string, std::shared_ptr<service_pool<cocaine::framework::app_service_t>>>
            clients_map_t;

    size_t m_pool_size;
    unsigned int m_reconnect_timeout;

    std::shared_ptr<cocaine::framework::service_manager_t> m_service_manager;
    clients_map_t m_services;
};

}} // namespace cocaine::proxy

#endif // COCAINE_NATIVE_PROXY_HPP

