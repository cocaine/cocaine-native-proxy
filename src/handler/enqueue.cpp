#include "cocaine/proxy/handler/enqueue.hpp"

#include <cocaine/framework/handlers/http.hpp>
#include <cocaine/framework/services/app.hpp>

#include "cocaine/proxy/log.hpp"
#include "cocaine/proxy/pool.hpp"
#include "cocaine/proxy/server.hpp"

namespace ph = std::placeholders;

using namespace cocaine::proxy::handler;

const char HEADER_X_COCAINE_SERVICE[] = "X-Cocaine-Service";
const char HEADER_X_COCAINE_EVENT[]   = "X-Cocaine-Event";

struct cocaine::proxy::handler::enqueue_t::route_t {
    std::string app;
    std::string event;

    std::string uri;
};

struct enqueue_t::response_stream_t:
    public framework::basic_stream<std::string>,
    public std::enable_shared_from_this<response_stream_t>
{
    const swarm::logger log;

    enqueue_t::route_t route;
    std::shared_ptr<enqueue_t> rq;

    enum class state_t {
        fresh,
        streaming
    };

    state_t state;
    std::atomic<bool> closed_;

    bool chunked;
    size_t content_length;

    std::mutex mutex;

public:
    explicit
    response_stream_t(std::shared_ptr<enqueue_t> rq, enqueue_t::route_t route):
        log(swarm::logger(rq->logger(), {{}})),
        route(std::move(route)),
        rq(std::move(rq)),
        state(state_t::fresh),
        closed_(false),
        chunked(false),
        content_length(0)
    {
        this->rq->checkpoint.loaded();
    }

    virtual
    void
    write(std::string&& chunk) {
        CP_DEBUG("> received %d byte chunk", chunk.size());

        std::unique_lock<std::mutex> lock(mutex);
        if (closed()) {
            return;
        }

        switch (state) {
        case state_t::fresh:
            rq->checkpoint.headers();
            write_headers(std::move(chunk), lock);
            state = state_t::streaming;
            break;
        case state_t::streaming:
            rq->checkpoint.body();
            write_body(std::move(chunk), lock);
            break;
        default:
            BOOST_ASSERT(false);
        }
    }

    virtual
    void
    error(const std::exception_ptr& err) {
        std::unique_lock<std::mutex> lock(mutex);
        error(err, lock);
    }

    virtual
    void
    close() {
        std::unique_lock<std::mutex> lock(mutex);
        close(boost::system::error_code(), lock);
    }

    virtual
    bool
    closed() const {
        return closed_;
    }

private:
    const swarm::logger&
    logger() const {
        return log;
    }

    void
    write_headers(std::string&& packed, const std::unique_lock<std::mutex>& lock) {
        CP_DEBUG("> writing headers");

        try {
            int code;
            framework::http_headers_t headers;
            std::tie(code, headers) = framework::unpack<std::tuple<int, framework::http_headers_t>>(packed);

            thevoid::http_response reply;
            reply.set_code(code);
            reply.set_headers(headers.data());

            if (auto content_length = reply.headers().content_length()) {
                this->chunked = false;
                this->content_length = *content_length;
            } else {
                chunked = true;
                reply.headers().set("Transfer-Encoding", "chunked");
            }

            reply.headers().set("X-Powered-By", "Cocaine");

            rq->send_headers(
                std::move(reply),
                std::bind(&response_stream_t::on_error, shared_from_this(), ph::_1)
            );
        } catch (const std::exception&) {
            error(std::current_exception(), lock);
        }
    }

    void
    write_body(std::string&& packed, const std::unique_lock<std::mutex>& lock) {
        CP_DEBUG("> writing body");

        try {
            std::string chunk = framework::unpack<std::string>(packed);

            if (chunk.size() != 0) {
                if (chunked) {
                    // TODO: O_o.
                    rq->send_data(
                        cocaine::format("%x\r\n%s\r\n", chunk.size(), std::move(chunk)),
                        std::bind(&response_stream_t::on_error, shared_from_this(), ph::_1)
                    );
                } else if (chunk.size() <= content_length) {
                    content_length -= chunk.size();
                    rq->send_data(
                        std::move(chunk),
                        std::bind(&response_stream_t::on_error, shared_from_this(), ph::_1)
                    );
                } else {
                    if (content_length != 0) {
                        rq->send_data(
                            chunk.substr(0, content_length),
                            std::bind(&response_stream_t::on_error, shared_from_this(), ph::_1)
                        );
                        content_length = 0;
                    }

                    CP_WARN("application has returned bytes more then 'content-length'");
                }
            }
        } catch (const std::exception&) {
            error(std::current_exception(), lock);
        }
    }

    void
    error(const std::exception_ptr& err, const std::unique_lock<std::mutex>& lock) {
        if (closed()) {
            return;
        }

        if (state == state_t::fresh) {
            try {
                std::rethrow_exception(err);
            } catch (const framework::service_error_t& err) {
                const auto& ec = err.code();

                if (ec.category() == framework::service_response_category()) {
                    CP_WARN("unable to finish request, application has responded with error: %s", err.what())(
                        "app", route.app, "event", route.event
                    );
                    rq->send_reply(thevoid::http_response::internal_server_error);
                } else if (ec == framework::service_errc::not_found) {
                    CP_WARN("unable to finish request: not found")(
                        "app", route.app, "event", route.event
                    );
                    rq->send_reply(thevoid::http_response::not_found);
                } else if (ec == framework::service_errc::not_connected) {
                    CP_WARN("unable to finish request: not connected")(
                        "app", route.app, "event", route.event
                    );
                    rq->send_reply(thevoid::http_response::bad_gateway);
                } else if (ec == framework::service_errc::timeout) {
                    CP_WARN("unable to finish request: timed out")(
                        "app", route.app, "event", route.event
                    );
                    rq->send_reply(thevoid::http_response::gateway_timeout);
                } else {
                    CP_WARN("unable to finish request: %s", err.what())(
                        "app", route.app, "event", route.event
                    );
                    rq->send_reply(thevoid::http_response::internal_server_error);
                }
            } catch (const std::exception& err) {
                CP_WARN("unable to finish request: %s", err.what())(
                    "app", route.app, "event", route.event
                );
                rq->send_reply(thevoid::http_response::internal_server_error);
            }

            // Reply has been sent, mark stream as closed and remove request.
            closed_ = true;
            dump(rq->checkpoint.dump());
            rq.reset();
        } else {
            try {
                std::rethrow_exception(err);
            } catch (const framework::service_error_t& err) {
                const auto& ec = err.code();

                if (ec.category() == framework::service_response_category()) {
                    CP_WARN("unable to finish request, application has responded with error: %s", err.what())(
                        "app", route.app, "event", route.event
                    );
                } else if (ec == framework::service_errc::not_connected) {
                    CP_WARN("unable to finish request: not connected")(
                        "app", route.app, "event", route.event
                    );
                } else {
                    CP_WARN("unable to finish request: %s", err.what())(
                        "app", route.app, "event", route.event
                    );
                }
            } catch (const std::exception& err) {
                CP_WARN("unable to finish request: %s", err.what())(
                    "app", route.app, "event", route.event
                );
            }

            // Headers have been sent before. Invocation of close() with empty error will send
            // response completion and closes reply stream.
            close(boost::system::error_code(), lock);
        }
    }

    void
    close(const boost::system::error_code& ec, const std::unique_lock<std::mutex>&) {
        if (closed()) {
            return;
        }

        closed_ = true;

        if (ec) {
            // The close() method is called with "non-empty" error code only in case of failed
            // write. In this case thevoid closes connection.
        } else if (state == state_t::streaming) {
            if (chunked) {
                rq->send_data(
                    std::string("0\r\n\r\n"),
                    std::bind(&thevoid::reply_stream::close, rq->reply(), ph::_1)
                );
            } else if (content_length != 0) {
                CP_WARN("application '%s' has returned on event '%s' less then 'content-length'",
                    route.app, route.event
                );

                rq->send_data(
                    std::string(),
                    std::bind(
                        &thevoid::reply_stream::close,
                        rq->reply(),
                        boost::system::errc::make_error_code(boost::system::errc::io_error)
                    )
                );
            } else {
                rq->send_data(
                    std::string(),
                    std::bind(&thevoid::reply_stream::close, rq->reply(), ph::_1)
                );
            }
        } else {
            // Here headers haven't been sent yet so send reply to client.
            rq->send_reply(boost::system::errc::io_error);
        }

        dump(rq->checkpoint.dump());
        rq.reset();
    }

    void
    on_error(const boost::system::error_code& ec) {
        if (ec) {
            CP_ERROR("error occurred while sending response: %s", ec.message());

            std::unique_lock<std::mutex> lock(mutex);
            close(ec, lock);
        }
    }

    void
    dump(const tracing_t::dump_t&) {
    }
};

enqueue_t::enqueue_t() {}

void
enqueue_t::on_request(const thevoid::http_request& rq, const boost::asio::const_buffer& buf) {
    CP_DEBUG("processing enqueue request")("url", rq.url().to_string());

    checkpoint.accepted();

    CP_DEBUG("extracting enqueue request route");
    if (auto route = extract_routing(rq)) {
        process(*route, rq, buf);
    } else {
        CP_WARN("unable to extract the routing: neither url convention matched nor valid headers found");
        reply()->send_error(thevoid::http_response::bad_request);
    }
}

void
enqueue_t::process(const route_t& route, const thevoid::http_request& rq, const boost::asio::const_buffer& buf) {
    CP_DEBUG("processing extracted route")("app", route.app, "event", route.event, "uri", route.uri);

    std::string body(boost::asio::buffer_cast<const char*>(buf), boost::asio::buffer_size(buf));

    try {
        // Obtain the service pool using the given app name.
        server()->pool(route.app)->next().then(
            std::bind(&enqueue_t::on_service, shared_from_this(), ph::_1, route, rq, std::move(body))
        );
    } catch (const framework::service_error_t& err) {
        const auto& ec = err.code();

        switch (static_cast<framework::service_errc>(ec.value())) {
        case framework::service_errc::not_found:
            CP_WARN("application not found")("app", route.app);
            reply()->send_error(thevoid::http_response::not_found);
            break;
        case framework::service_errc::not_connected:
            CP_WARN("unable to connect to the Locator")("app", route.app);
            reply()->send_error(thevoid::http_response::bad_gateway);
            break;
        default:
            CP_WARN("unable to connect to the app: [%d] %s", ec.value(), ec.message())("app", route.app);
            reply()->send_error(thevoid::http_response::internal_server_error);
        }
    } catch (const std::exception& err) {
        CP_WARN("unable to connect to the app: %s", err.what())("app", route.app);
        reply()->send_error(thevoid::http_response::internal_server_error);
    } catch (...) {
        CP_WARN("unable to connect to the app: unknown error")("app", route.app);
        reply()->send_error(thevoid::http_response::internal_server_error);
    }
}

void
enqueue_t::on_service(service_future_type& future, route_t route, thevoid::http_request rq, std::string body) {
    CP_DEBUG("sending enqueue event")("app", route.app, "event", route.event, "uri", route.uri);

    try {
        auto service = future.get();
        service->set_timeout(5.0f);

        std::string http_version = cocaine::format(
            "%d.%d",
            rq.http_major_version(),
            rq.http_minor_version()
        );

        CP_DEBUG("http version: %s", http_version);

        service->enqueue(
            route.event,
            framework::http_request_t(
                rq.method(),
                route.uri,
                http_version,
                framework::http_headers_t(rq.headers().all()),
                body
            )
        ).redirect(std::make_shared<response_stream_t>(shared_from_this(), route));
    } catch (const std::exception& err) {
        CP_WARN("unable to send enqueue event: %s", err.what())("app", route.app, "event", route.event);
        reply()->send_error(thevoid::http_response::internal_server_error);
    } catch (...) {
        CP_WARN("unable to send enqueue event: unknown error")("app", route.app, "event", route.event);
        reply()->send_error(thevoid::http_response::internal_server_error);
    }
}

boost::optional<enqueue_t::route_t>
enqueue_t::extract_routing(const thevoid::http_request &rq) {
    auto app   = rq.headers().get(HEADER_X_COCAINE_SERVICE);
    auto event = rq.headers().get(HEADER_X_COCAINE_EVENT);

    if (app && event) {
        return route_t { std::move(*app), std::move(*event), rq.url().original() };
    } else {
        // Parse url to extract an application name and its event, like http://host/application/event[?/...]
        const std::string path = rq.url().original();

        std::size_t start = path.find('/');
        if (start != std::string::npos) {
            std::size_t end = path.find('/', start + 1);
            if (end != std::string::npos) {
                route_t route;
                route.app = path.substr(start + 1, end - start - 1);

                start = end;
                end = path.find_first_of("?/", start + 1);

                route.event = path.substr(start + 1, end - start - 1);
                route.uri   = path.substr(std::min(end, path.size()));

                return route;
            }
        }
    }

    return boost::none;
}
