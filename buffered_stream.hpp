#ifndef BUFFERED_STREAM_HPP
#define BUFFERED_STREAM_HPP

#include <thevoid/stream.hpp>

#include <cocaine/framework/logging.hpp>

#include <string>
#include <queue>
#include <mutex>
#include <memory>

namespace cocaine { namespace proxy {

class buffered_stream_t :
    public std::enable_shared_from_this<buffered_stream_t>
{
public:
    buffered_stream_t(std::shared_ptr<ioremap::thevoid::reply_stream> output,
                      std::shared_ptr<cocaine::framework::logger_t> logger);

    void
    set_headers(const ioremap::swarm::network_reply& headers);

    void
    push(std::string&& chunk);

    void
    close(const boost::system::error_code &err = boost::system::error_code());

private:
    void
    on_sent(const boost::system::error_code&);

private:
    std::shared_ptr<ioremap::thevoid::reply_stream> m_output;
    std::mutex m_send_lock;

    std::shared_ptr<cocaine::framework::logger_t> m_logger;

    std::queue<std::string> m_chunks;
    bool m_closed;
    boost::system::error_code m_err_code;
};

}} // namespace cocaine::proxy

#endif // BUFFERED_STREAM_HPP

