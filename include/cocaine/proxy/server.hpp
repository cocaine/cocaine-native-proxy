/*
    Copyright (c) 2013 Andrey Goryachev <andrey.goryachev@gmail.com>
    Copyright (c) 2015 Evgeny Safronov  <division494@gmail.com>
    Copyright (c) 2011-2015 Other contributors as noted in the AUTHORS file.

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

#pragma once

#include <chrono>
#include <cstdint>

#include <thevoid/server.hpp>

#include "cocaine/proxy/common.hpp"

namespace cocaine { namespace framework {
    class app_service_t;
    class service_manager_t;
}}

namespace cocaine { namespace proxy {

template<class T> class service_pool;

class server_t:
    public thevoid::server<server_t>
{
    typedef framework::app_service_t   service_type;
    typedef service_pool<service_type> pool_type;

    typedef std::map<
        std::string,
        std::shared_ptr<pool_type>
    > pool_map_type;

    std::size_t pool_size;
    std::uint32_t reconnect_timeout;
    std::uint32_t request_timeout;

    std::shared_ptr<framework::service_manager_t> service_manager;

    pool_map_type services;
    mutable std::mutex mutex;

public:
    ~server_t();

    bool
    initialize(const rapidjson::Value& config);

    std::map<std::string, std::string>
    get_statistics() const;

    std::shared_ptr<pool_type>
    pool(const std::string& app);
};

}} // namespace cocaine::proxy

