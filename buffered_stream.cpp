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
        m_output.reset();
        m_logger.reset();
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
        m_output.reset();
        m_logger.reset();
    }
}
