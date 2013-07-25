#include "proxy.hpp"

#include <swarm/network_url.h>
#include <swarm/network_query_list.h>

#include <thevoid/rapidjson/stringbuffer.h>
#include <thevoid/rapidjson/prettywriter.h>

#include <msgpack.hpp>

#include <cocaine/traits/literal.hpp>
#include <cocaine/asio/resolver.hpp>
#include <cocaine/framework/handlers/http.hpp>

#include <boost/system/linux_error.hpp>

#include <iostream>
#include <sstream>
#include <utility>

#include <sys/types.h>
#include <unistd.h>
#include <csignal>

using namespace ioremap::swarm;
using namespace ioremap::thevoid;
using namespace cocaine::proxy;

namespace ph = std::placeholders;
namespace cf = cocaine::framework;

bool
proxy::initialize(const rapidjson::Value &config) {
    if (!config.HasMember("locator")) {
        std::cerr << "'locator' field is missed";
        return false;
    }

    std::string locator = config["locator"].GetString();

    // kostyl-way!
    size_t delim = locator.rfind(':');
    if (delim == std::string::npos) {
        std::cerr << "Bad format of locator's endpoint" << std::endl;
        return false;
    }

    std::string host = locator.substr(0, delim);
    uint16_t port;
    std::istringstream port_parser(locator.substr(delim + 1));
    if (!(port_parser >> port)) {
        std::cerr << "Bad format of locator's endpoint" << std::endl;
        return false;
    }

    std::string logging_prefix = "native-proxy";

    if (config.HasMember("logging_prefix")) {
        logging_prefix = config["logging_prefix"].GetString();
    }

    m_service_manager = cf::service_manager_t::create(
        cocaine::io::resolver<cocaine::io::tcp>::query(host, port),
        cocaine::format("%s/%d", logging_prefix, getpid())
    );

    m_pool_size = 10;

    if (config.HasMember("service_pool")) {
        m_pool_size = config["service_pool"].GetUint();
    }

    m_reconnect_timeout = 180;

    if (config.HasMember("reconnect_timeout")) {
        m_reconnect_timeout = config["reconnect_timeout"].GetUint();
    }

    on_prefix<on_enqueue>("/");

    return true;
}

proxy::~proxy() {
    m_service_manager->stop();
}

void
proxy::on_enqueue::on_request(const network_request &req,
                              const boost::asio::const_buffer &body)
{
    m_buffered = std::make_shared<buffered_stream_t>(
        get_reply(),
        get_server()->m_service_manager->get_system_logger()
    );
    m_chunked = false;

    // Parse url to extract application name and event (format http://host/application/event[?/]...)
    // a little more kostyl-way
    size_t start = req.get_url().find('/');
    if (start != std::string::npos) {
        size_t end = req.get_url().find('/', start + 1);
        if (end != std::string::npos) {
            m_application = req.get_url().substr(start + 1, end - start - 1);
            start = end;
            end = std::min(req.get_url().find('/', start + 1),
                           req.get_url().find('?', start + 1));
            m_event = req.get_url().substr(start + 1, end - start - 1);

            std::string http_version = cocaine::format("%d.%d",
                                                       req.get_http_major_version(),
                                                       req.get_http_minor_version());


            // Create connection to the application if one doesn't exist
            proxy::clients_map_t::iterator it = get_server()->m_services.find(m_application);
            if (it == get_server()->m_services.end()) {
                try {
                    it = get_server()->m_services.insert(std::make_pair(
                        m_application,
                        std::make_shared<service_pool<cf::app_service_t>>(
                            get_server()->m_pool_size,
                            get_server()->m_reconnect_timeout,
                            get_server()->m_service_manager,
                            m_application
                        )
                    )).first;
                } catch (...) {
                    get_reply()->send_error(network_reply::not_found);

                    COCAINE_LOG_INFO(get_server()->m_service_manager->get_system_logger(),
                                     "Application '%s' not found. Url - %s",
                                     m_application,
                                     req.get_url());

                    return;
                }
            }

            // send request
            try {
                (*it->second)->enqueue(
                    m_event,
                    cf::http_request_t (
                        req.get_method(),
                        req.get_url(),
                        http_version,
                        cf::http_headers_t(req.get_headers()),
                        std::string (
                            boost::asio::buffer_cast<const char*>(body),
                            boost::asio::buffer_size(body)
                        )
                    )
                ).then(std::bind(&on_enqueue::on_resp_headers, shared_from_this(), ph::_1));
            } catch (...) {
                get_reply()->send_error(network_reply::internal_server_error);
            }

            // Prepend sending of error in the bottom of method!
            return;
        }
    }

    COCAINE_LOG_INFO(get_server()->m_service_manager->get_system_logger(),
                     "Unable to parse url '%s'",
                     req.get_url());

    get_reply()->send_error(network_reply::not_found);
}

void
proxy::on_enqueue::on_resp_headers(cf::generator<std::string>& g) {
    try {
        std::string chunk = g.next();

        int code;
        cf::http_headers_t headers;

        std::tie(code, headers) = cf::unpack<std::tuple<int, cf::http_headers_t>>(chunk);

        ioremap::swarm::network_reply reply;
        reply.set_code(code);
        reply.set_headers(headers.data());

        if (reply.has_content_length()) {
            m_chunked = false;
            m_content_length = reply.get_content_length();
        } else {
            m_chunked = true;
            reply.set_header("Transfer-Encoding", "chunked");
        }

        m_buffered->set_headers(reply);

        g.map(std::bind(&on_enqueue::on_resp_chunk, shared_from_this(), ph::_1))
         .gather().then(std::bind(&on_enqueue::on_resp_close, shared_from_this(), ph::_1));
    } catch (...) {
        get_reply()->send_error(ioremap::swarm::network_reply::internal_server_error);

        COCAINE_LOG_INFO(get_server()->m_service_manager->get_system_logger(),
                         "Application '%s' has returned error on event '%s'",
                         m_application,
                         m_event);
    }
}

void
proxy::on_enqueue::on_resp_chunk(cf::future<std::string>& f) {
    try {
        std::string chunk = cf::unpack<std::string>(f.get());
        if (chunk.size() != 0) {
            if (m_chunked) {
                m_buffered->push(cocaine::format("%x\r\n%s\r\n", chunk.size(), std::move(chunk)));
            } else if (chunk.size() <= m_content_length) {
                m_content_length -= chunk.size();
                m_buffered->push(std::move(chunk));
            } else {
                if (m_content_length != 0) {
                    m_buffered->push(chunk.substr(0, m_content_length));
                    m_content_length = 0;
                }

                COCAINE_LOG_WARNING(get_server()->m_service_manager->get_system_logger(),
                                    "Application '%s' has returned on event '%s' more then 'content-length'",
                                    m_application,
                                    m_event);
            }
        }
    } catch (...) {
        COCAINE_LOG_WARNING(get_server()->m_service_manager->get_system_logger(),
                            "Application '%s' has canceled session with error (event %s)",
                            m_application,
                            m_event);
    }
}

void
proxy::on_enqueue::on_resp_close(cf::future<std::vector<cf::future<void>>>&) {
    if (m_chunked) {
        m_buffered->push("0\r\n\r\n");
    } else if (m_content_length != 0) {
        m_buffered->close(boost::system::error_code(boost::system::linux_error::remote_io_error));
        COCAINE_LOG_WARNING(get_server()->m_service_manager->get_system_logger(),
                            "Application '%s' has returned on event '%s' less then 'content-length'",
                            m_application,
                            m_event);
        return;
    }
    m_buffered->close();
}


int main(int argc,
         char **argv)
{
    // Block the deprecated signals.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGPIPE);
    sigprocmask(SIG_BLOCK, &signals, nullptr);

    return run_server<proxy>(argc, argv);
}
