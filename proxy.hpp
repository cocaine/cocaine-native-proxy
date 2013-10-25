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
    struct on_enqueue;

    struct response_stream :
        public cocaine::framework::basic_stream<std::string>,
        public std::enable_shared_from_this<response_stream>
    {
        response_stream(const std::shared_ptr<on_enqueue>& req);

        virtual
        void
        write(std::string&& chunk);

        void
        write_headers(std::string&& packed);

        void
        write_body(std::string&& packed);

        virtual
        void
        error(const std::exception_ptr& e);

        virtual
        void
        close();

        virtual
        bool
        closed() const {
            return m_closed;
        }

    private:
        std::shared_ptr<on_enqueue> m_request;
        std::shared_ptr<buffered_stream_t> m_buffered;

        bool m_chunked;
        size_t m_content_length;
        bool m_body;

        std::atomic<bool> m_closed;
    };
    friend struct response_stream;

    struct on_enqueue :
        public ioremap::thevoid::simple_request_stream<proxy>,
        public std::enable_shared_from_this<on_enqueue>
    {
        friend struct response_stream;

        virtual
        void
        on_request(const ioremap::swarm::network_request &req,
                   const boost::asio::const_buffer &buffer);

        const std::string&
        app() const {
            return m_application;
        }

        const std::string&
        event() const {
            return m_event;
        }

    private:
        std::string m_application;
        std::string m_event;
    };
    friend struct on_enqueue;

public:
    bool
    initialize(const rapidjson::Value &config);

    ~proxy();

    std::map<std::string, std::string>
    statistics() const;

private:
    typedef std::map<std::string, std::shared_ptr<service_pool<cocaine::framework::app_service_t>>>
            clients_map_t;

    size_t m_pool_size;
    unsigned int m_reconnect_timeout;
    unsigned int m_request_timeout;

    std::shared_ptr<cocaine::framework::service_manager_t> m_service_manager;
    clients_map_t m_services;
    mutable std::mutex m_services_mutex;
};

}} // namespace cocaine::proxy

#endif // COCAINE_NATIVE_PROXY_HPP

