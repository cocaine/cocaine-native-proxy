/*
    Copyright (c) 2013 Andrey Goryachev <andrey.goryachev@gmail.com>
    Copyright (c) 2011-2013 Other contributors as noted in the AUTHORS file.

    This file is part of Cocaine.

    Cocaine is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    Cocaine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "proxy.hpp"

#include <swarm/url.hpp>

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
    if (!config.HasMember("locators")) {
        std::cerr << "'locators' field is missed in config. You should specify locators as a list of strings in format 'host:port'." << std::endl;
        return false;
    }

    auto& locators = config["locators"];

    if (!locators.IsArray()) {
        std::cerr << "Locators must be specified as an array of endpoints." << std::endl;
        return false;
    }

    std::vector<cf::service_manager_t::endpoint_t> unpacked_locators;

    for (size_t i = 0; i < locators.Size(); ++i) {
        if (!locators[i].IsString()) {
            std::cerr << "Bad format of locator's endpoint. It must be a string in format 'host:port'." << std::endl;
            return false;
        }

        std::string locator = locators[i].GetString();

        // kostyl-way!
        size_t delim = locator.rfind(':');
        if (delim == std::string::npos) {
            std::cerr << "Bad format of locator's endpoint. Format: host:port." << std::endl;
            return false;
        }

        std::string host = locator.substr(0, delim);
        uint16_t port;
        std::istringstream port_parser(locator.substr(delim + 1));
        if (!(port_parser >> port)) {
            std::cerr << "Bad format of locator's endpoint. Format: host:port." << std::endl;
            return false;
        }

        unpacked_locators.emplace_back(host, port);
    }

    if (unpacked_locators.empty()) {
        std::cerr << "List of locators is empty. Specify some ones." << std::endl;
        return false;
    }


    std::string logging_prefix = "native-proxy";

    if (config.HasMember("logging_prefix")) {
        logging_prefix = config["logging_prefix"].GetString();
    }

    size_t threads_num = threads_count();

    if (config.HasMember("threads")) {
        threads_num = config["threads"].GetUint();
    }

    m_service_manager = cf::service_manager_t::create(unpacked_locators, logging_prefix, threads_num);

    COCAINE_LOG_INFO(m_service_manager->get_system_logger(),
                     "Proxy has successfully started.");

    m_pool_size = 10;

    if (config.HasMember("service_pool")) {
        m_pool_size = config["service_pool"].GetUint();
    }

    m_reconnect_timeout = 180;

    if (config.HasMember("reconnect_timeout")) {
        m_reconnect_timeout = config["reconnect_timeout"].GetUint();
    }

    m_request_timeout = 5;

    if (config.HasMember("request_timeout")) {
        m_request_timeout = config["request_timeout"].GetUint();
    }

    on<on_ping>(options::prefix_match("/ping"));
    on<on_enqueue>(options::prefix_match("/"));

    return true;
}

proxy::~proxy() {
    COCAINE_LOG_INFO(m_service_manager->get_system_logger(),
                     "Proxy will be stopped now.");

    m_services.clear();
    m_service_manager.reset();
}

void
proxy::on_ping::on_request(const http_request & /*request*/,
                           const boost::asio::const_buffer & /*body*/)
{
    send_reply(http_response::ok);
}

void
proxy::on_enqueue::on_request(const http_request &req,
                              const boost::asio::const_buffer &body)
{
    COCAINE_LOG_DEBUG(server()->m_service_manager->get_system_logger(),
                      "Request has accepted: %s",
                      req.url().to_string());

    bool destination_found = false;

    std::string uri;

    auto app = req.headers().get("X-Cocaine-Service");
    auto event = req.headers().get("X-Cocaine-Event");
    if (app && event) {
        m_application = std::move(*app);
        m_event = std::move(*event);
        uri = req.url().path();
        destination_found = true;
    } else {
        // Parse url to extract application name and event (in format http://host/application/event[?/]...).
        const std::string path = req.url().original();

        size_t start = path.find('/');
        if (start != std::string::npos) {
            size_t end = path.find('/', start + 1);
            if (end != std::string::npos) {
                m_application = path.substr(start + 1, end - start - 1);
                start = end;
                end = std::min(path.find('/', start + 1),
                               path.find('?', start + 1));
                m_event = path.substr(start + 1, end - start - 1);
                uri = path.substr(std::min(end, path.size()));
                destination_found = true;
            }
        }
    }

    if (destination_found) {
        COCAINE_LOG_DEBUG(server()->m_service_manager->get_system_logger(),
                          "Request '%s' will be sent to application '%s' with event '%s'.",
                          uri,
                          m_application,
                          m_event);

        proxy::clients_map_t::iterator it;
        { // critical section
            std::lock_guard<std::mutex> guard(server()->m_services_mutex);
            it = server()->m_services.find(m_application);

            // Create connection to the application if one doesn't exist
            if (it == server()->m_services.end()) {
                try {
                    it = server()->m_services.insert(std::make_pair(
                        m_application,
                        std::make_shared<service_pool<cf::app_service_t>>(
                            server()->m_pool_size,
                            server()->m_reconnect_timeout,
                            server()->m_service_manager,
                            server()->m_request_timeout,
                            m_application
                        )
                    )).first;
                } catch (const cf::service_error_t& e) {
                    if (e.code().category() == cf::service_client_category() &&
                        e.code().value() == static_cast<int>(cf::service_errc::not_found))
                    {
                        get_reply()->send_error(http_response::not_found);

                        COCAINE_LOG_INFO(server()->m_service_manager->get_system_logger(),
                                         "Application '%s' not found in the cloud. Url - '%s'.",
                                         m_application,
                                         req.url().to_string());
                    } else if (e.code().category() == cf::service_client_category() &&
                               e.code().value() == static_cast<int>(cf::service_errc::not_connected))
                    {
                        get_reply()->send_error(http_response::bad_gateway);

                        COCAINE_LOG_WARNING(server()->m_service_manager->get_system_logger(),
                                            "Unable to connect to the locator. How have i logged it? WTF?!");
                    } else {
                        get_reply()->send_error(http_response::internal_server_error);

                        COCAINE_LOG_WARNING(server()->m_service_manager->get_system_logger(),
                                            "Error has occurred while connecting to application '%s' (url - '%s'): %s; code - %d",
                                            m_application,
                                            req.url().to_string(),
                                            e.what(),
                                            e.code().value());
                    }

                    return;
                } catch (const std::exception& e) {
                    get_reply()->send_error(http_response::internal_server_error);

                    COCAINE_LOG_WARNING(server()->m_service_manager->get_system_logger(),
                                        "Error has occurred while connecting to application '%s' (url - '%s'): %s",
                                        m_application,
                                        req.url().to_string(),
                                        e.what());

                    return;
                } catch (...) {
                    get_reply()->send_error(http_response::internal_server_error);

                    COCAINE_LOG_WARNING(server()->m_service_manager->get_system_logger(),
                                        "Unknown error has occurred while connecting to application '%s' (url - '%s')",
                                        m_application,
                                        req.url().to_string());

                    return;
                }
            }
        } // critical section

        // send request
        try {
            std::string http_version = cocaine::format("%d.%d",
                                                       req.http_major_version(),
                                                       req.http_minor_version());

            (*it->second)->enqueue(
                m_event,
                cf::http_request_t (
                    req.method(),
                    uri,
                    http_version,
                    cf::http_headers_t(req.headers().all()),
                    std::string (
                        boost::asio::buffer_cast<const char*>(body),
                        boost::asio::buffer_size(body)
                    )
                )
            ).redirect(std::make_shared<proxy::response_stream>(shared_from_this()));
        } catch (const std::exception& e) {
            get_reply()->send_error(http_response::internal_server_error);

            COCAINE_LOG_WARNING(server()->m_service_manager->get_system_logger(),
                                "Error has occurred while enqueue event '%s' to application '%s': %s",
                                m_event,
                                m_application,
                                e.what());
        } catch (...) {
            get_reply()->send_error(http_response::internal_server_error);

            COCAINE_LOG_WARNING(server()->m_service_manager->get_system_logger(),
                                "Unknown error has occurred while enqueue event '%s' to application '%s'",
                                m_event,
                                m_application);
        }
    } else {
        COCAINE_LOG_INFO(server()->m_service_manager->get_system_logger(),
                         "Unable to extract destination from headers or from url '%s'.",
                         req.url().to_string());

        get_reply()->send_error(http_response::not_found);
    }
}

proxy::response_stream::response_stream(const std::shared_ptr<on_enqueue>& req) :
    m_request(req),
    m_body(false),
    m_closed(false)
{
    // pass
}

void
proxy::response_stream::write(std::string&& chunk) {
    if (!closed()) {
        if (!m_body) {
            write_headers(std::move(chunk));
            m_body = true;
        } else {
            write_body(std::move(chunk));
        }
    }
}

void
proxy::response_stream::error(const std::exception_ptr& e) {
    if (!closed()) {
        if (!m_body) {
            try {
                std::rethrow_exception(e);
            } catch (const cf::service_error_t& e) {
                if (e.code().category() == cf::service_response_category()) {
                    m_request->get_reply()->send_error(http_response::internal_server_error);

                    COCAINE_LOG_WARNING(m_request->server()->m_service_manager->get_system_logger(),
                                        "Application '%s' returned error on event '%s': %s; code - %d.",
                                        m_request->app(),
                                        m_request->event(),
                                        e.what(),
                                        e.code().value());
                } else if (e.code().value() == static_cast<int>(cf::service_errc::not_found)) {
                    m_request->get_reply()->send_error(http_response::not_found);

                    COCAINE_LOG_INFO(m_request->server()->m_service_manager->get_system_logger(),
                                     "Application '%s' not found in cloud.",
                                     m_request->app());
                } else if (e.code().value() == static_cast<int>(cf::service_errc::not_connected)) {
                    m_request->get_reply()->send_error(http_response::bad_gateway);

                    COCAINE_LOG_WARNING(m_request->server()->m_service_manager->get_system_logger(),
                                        "Unable to connect to application '%s'.",
                                        m_request->app());
                } else if (e.code().value() == static_cast<int>(cf::service_errc::timeout)) {
                    m_request->get_reply()->send_error(http_response::gateway_timeout);

                    COCAINE_LOG_WARNING(m_request->server()->m_service_manager->get_system_logger(),
                                        "Request '%s' to application '%s' has timed out.",
                                        m_request->event(),
                                        m_request->app());
                } else {
                    m_request->get_reply()->send_error(http_response::internal_server_error);

                    COCAINE_LOG_WARNING(m_request->server()->m_service_manager->get_system_logger(),
                                        "Internal error has occurred while processing event '%s' of application '%s': %s; code - %d.",
                                        m_request->event(),
                                        m_request->app(),
                                        e.what(),
                                        e.code().value());
                }

                return;
            } catch (const std::exception& e) {
                m_request->get_reply()->send_error(ioremap::swarm::http_response::internal_server_error);

                COCAINE_LOG_WARNING(m_request->server()->m_service_manager->get_system_logger(),
                                    "Internal error has occurred while processing event '%s' of application '%s': %s",
                                    m_request->event(),
                                    m_request->app(),
                                    e.what());
            }
        } else {
            try {
                std::rethrow_exception(e);
            } catch (const cf::service_error_t& e) {
                if (e.code().category() == cf::service_response_category()) {
                    COCAINE_LOG_WARNING(m_request->server()->m_service_manager->get_system_logger(),
                                        "Application '%s' returned error while processing event '%s': %s; code - %d.",
                                        m_request->app(),
                                        m_request->event(),
                                        e.what(),
                                        e.code().value());
                } else if (e.code().value() == static_cast<int>(cf::service_errc::not_connected)) {
                    COCAINE_LOG_WARNING(m_request->server()->m_service_manager->get_system_logger(),
                                        "Connection to application '%s' has been lost while processing event '%s'.",
                                        m_request->app(),
                                        m_request->event());
                } else {
                    COCAINE_LOG_WARNING(m_request->server()->m_service_manager->get_system_logger(),
                                        "Internal error has occurred while processing event '%s' of application '%s': %s; code - %d.",
                                        m_request->event(),
                                        m_request->app(),
                                        e.what(),
                                        e.code().value());
                }
            } catch (const std::exception& e) {
                COCAINE_LOG_WARNING(m_request->server()->m_service_manager->get_system_logger(),
                                    "Internal error has occurred while processing event '%s' of application '%s': %s",
                                    m_request->event(),
                                    m_request->app(),
                                    e.what());
            }
        }
        close();
    }
}

void
proxy::response_stream::close(const boost::system::error_code& ec) {
    if (!closed()) {
        m_closed = true;

        if (ec) {
            m_request->get_reply()->close(ec);
        } else if (m_body) {
            if (m_chunked) {
                m_request->send_data(std::string("0\r\n\r\n"),
                                     std::bind(&reply_stream::close, m_request->get_reply(), std::placeholders::_1));
            } else if (m_content_length != 0) {
                m_request->get_reply()->close(boost::system::error_code(boost::system::linux_error::remote_io_error));
                COCAINE_LOG_WARNING(m_request->server()->m_service_manager->get_system_logger(),
                                    "Application '%s' has returned on event '%s' less then 'content-length'",
                                    m_request->app(),
                                    m_request->event());
            } else {
                m_request->send_data(std::string(),
                                     std::bind(&reply_stream::close, m_request->get_reply(), std::placeholders::_1));
            }
        }

        m_request.reset();
    }
}

void proxy::response_stream::on_error(const boost::system::error_code &ec) {
    if (ec) {
        COCAINE_LOG_INFO(m_request->server()->m_service_manager->get_system_logger(),
                         "Error has occurred while sending response: %s", ec.message());
    
        close(ec);
    }
}

void
proxy::response_stream::write_headers(std::string&& packed) {
    try {
        int code;
        cf::http_headers_t headers;
        std::tie(code, headers) = cf::unpack<std::tuple<int, cf::http_headers_t>>(packed);

        ioremap::swarm::http_response reply;
        reply.set_code(code);
        reply.set_headers(headers.data());

        if (auto content_length = reply.headers().content_length()) {
            m_chunked = false;
            m_content_length = *content_length;
        } else {
            m_chunked = true;
            reply.headers().set("Transfer-Encoding", "chunked");
        }

        reply.headers().set("X-Powered-By", "Cocaine");

        m_request->send_headers(std::move(reply),
                                std::bind(&response_stream::on_error, shared_from_this(), std::placeholders::_1));
    } catch (const std::exception& e) {
        error(std::current_exception());
    }
}

void
proxy::response_stream::write_body(std::string&& packed) {
    try {
        std::string chunk = cf::unpack<std::string>(packed);
        if (chunk.size() != 0) {
            if (m_chunked) {
                m_request->send_data(cocaine::format("%x\r\n%s\r\n", chunk.size(), std::move(chunk)),
                                     std::bind(&response_stream::on_error, shared_from_this(), std::placeholders::_1));
            } else if (chunk.size() <= m_content_length) {
                m_content_length -= chunk.size();
                m_request->send_data(std::move(chunk),
                                     std::bind(&response_stream::on_error, shared_from_this(), std::placeholders::_1));
            } else {
                if (m_content_length != 0) {
                    m_request->send_data(chunk.substr(0, m_content_length),
                                         std::bind(&response_stream::on_error, shared_from_this(), std::placeholders::_1));
                    m_content_length = 0;
                }

                COCAINE_LOG_WARNING(m_request->server()->m_service_manager->get_system_logger(),
                                    "Application '%s' has returned on event '%s' more then 'content-length'",
                                    m_request->app(),
                                    m_request->event());
            }
        }
    } catch (const std::exception& e) {
        error(std::current_exception());
    }
}

std::map<std::string, std::string>
proxy::get_statistics() const {
    std::map<std::string, std::string> stat;

    stat["memory"] = boost::lexical_cast<std::string>(m_service_manager->footprint());
    stat["connections_count"] = boost::lexical_cast<std::string>(m_service_manager->connections_count());
    stat["sessions_count"] = boost::lexical_cast<std::string>(m_service_manager->sessions_count());

    {
        std::lock_guard<std::mutex> guard(m_services_mutex);

        stat["applications_count"] = boost::lexical_cast<std::string>(m_services.size());

        size_t connected_clients = 0;
        for (auto it = m_services.begin(); it != m_services.end(); ++it) {
            connected_clients += it->second->connected_clients();
        }

        stat["connected_clients"] = boost::lexical_cast<std::string>(connected_clients);
    }

    return stat;
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
