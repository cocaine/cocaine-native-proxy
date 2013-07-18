#include "buffered_stream.hpp"

#include <functional>

namespace ph = std::placeholders;

using namespace cocaine::proxy;

buffered_stream_t::buffered_stream_t(std::shared_ptr<ioremap::thevoid::reply_stream> output,
                                     std::shared_ptr<cocaine::framework::logger_t> logger) :
    m_output(output),
    m_logger(logger),
    m_closed(false)
{
    // pass
}

void
buffered_stream_t::set_headers(const ioremap::swarm::network_reply& headers) {
    std::unique_lock<std::mutex> lock(m_send_lock);

    // Just unification. One piece of data - one chunk in queue.
    m_chunks.emplace(std::string());
    m_output->send_headers(headers,
                           boost::asio::const_buffer(),
                           std::bind(&buffered_stream_t::on_sent, shared_from_this(), ph::_1));
}

void
buffered_stream_t::push(std::string&& chunk) {
    std::unique_lock<std::mutex> lock(m_send_lock);
    m_chunks.emplace(std::move(chunk));
    if (m_chunks.size() == 1) {
        m_output->send_data(
            boost::asio::const_buffer(m_chunks.front().data(), m_chunks.front().size()),
            std::bind(&buffered_stream_t::on_sent, shared_from_this(), ph::_1)
        );
    }
}

void
buffered_stream_t::close(const boost::system::error_code &err) {
    std::unique_lock<std::mutex> lock(m_send_lock);
    m_closed = true;
    m_err_code = err;
    if (m_chunks.empty()) {
        m_output->close(m_err_code);
    }
}

void
buffered_stream_t::on_sent(const boost::system::error_code& e) {
    if (e) {
        COCAINE_LOG_INFO(m_logger, "Error has occurred while sending response: %s", e.message());
    }

    std::unique_lock<std::mutex> lock(m_send_lock);
    m_chunks.pop();
    if (!m_chunks.empty()) {
        m_output->send_data(
            boost::asio::const_buffer(m_chunks.front().data(), m_chunks.front().size()),
            std::bind(&buffered_stream_t::on_sent, shared_from_this(), ph::_1)
        );
    } else if (m_closed) {
        m_output->close(m_err_code);
    }
}
