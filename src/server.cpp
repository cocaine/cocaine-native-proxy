/*
    Copyright (c) 2013 Andrey Goryachev <andrey.goryachev@gmail.com>
    Copyright (c) 2015 Evgeny Safronov <division494@gmail.com>
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

#define BLACKHOLE_CHECK_FORMAT_SYNTAX 0

#include "cocaine/proxy/server.hpp"

#include <iostream>
#include <sstream>

#include <cocaine/framework/services/app.hpp>

#include "cocaine/proxy/handler/enqueue.hpp"
#include "cocaine/proxy/handler/ping.hpp"
#include "cocaine/proxy/log.hpp"
#include "cocaine/proxy/pool.hpp"

using namespace cocaine;
using namespace cocaine::proxy;

bool
server_t::initialize(const rapidjson::Value& config) {
    if (!config.HasMember("locators")) {
        std::cerr << "'locators' field is missed in config. You should specify locators as a list of strings in format 'host:port'." << std::endl;
        return false;
    }

    auto& locators = config["locators"];

    if (!locators.IsArray()) {
        std::cerr << "Locators must be specified as an array of endpoints." << std::endl;
        return false;
    }

    std::vector<framework::service_manager_t::endpoint_t> endpoints;

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
        std::uint16_t port;
        std::istringstream port_parser(locator.substr(delim + 1));
        if (!(port_parser >> port)) {
            std::cerr << "Bad format of locator's endpoint. Format: host:port." << std::endl;
            return false;
        }

        endpoints.emplace_back(host, port);
    }

    if (endpoints.empty()) {
        std::cerr << "List of locators is empty. Specify some ones." << std::endl;
        return false;
    }

    std::string prefix = "native-proxy";
    if (config.HasMember("logging_prefix") && config["logging_prefix"].IsString()) {
        prefix = config["logging_prefix"].GetString();
    }
    CP_DEBUG("successfully parsed logging prefix field: %s", prefix);

    std::size_t threads = threads_count();
    if (config.HasMember("threads")) {
        if (config["threads"].IsUint() && config["threads"].GetUint() > 0) {
            threads = config["threads"].GetUint();
        } else {
            CP_ERROR("failed to parse config: thread number field must be a positive integer");
            return false;
        }
    }
    CP_DEBUG("successfully parsed thread number field: %d", threads);

    service_manager = framework::service_manager_t::create(endpoints, prefix, threads);

    pool_size = 10;
    if (config.HasMember("service_pool")) {
        pool_size = config["service_pool"].GetUint();
    }

    reconnect_timeout = 180;
    if (config.HasMember("reconnect_timeout")) {
        reconnect_timeout = config["reconnect_timeout"].GetUint();
    }

    request_timeout = 5;
    if (config.HasMember("request_timeout")) {
        request_timeout = config["request_timeout"].GetUint();
    }

    CP_INFO("proxy has successfully started");

    on<handler::ping_t>   (options::prefix_match("/ping"));
    on<handler::enqueue_t>(options::prefix_match("/"));

    return true;
}

server_t::~server_t() {
    CP_INFO("proxy will be stopped now");
}

std::map<std::string, std::string>
server_t::get_statistics() const {
    std::map<std::string, std::string> result;

    result["memory"]            = boost::lexical_cast<std::string>(service_manager->footprint());
    result["connections_count"] = boost::lexical_cast<std::string>(service_manager->connections_count());
    result["sessions_count"]    = boost::lexical_cast<std::string>(service_manager->sessions_count());

    std::lock_guard<std::mutex> lock(mutex);
    result["applications_count"] = boost::lexical_cast<std::string>(services.size());

    size_t total = 0;
    for (auto it = services.begin(); it != services.end(); ++it) {
        total += it->second->clients();
    }

    result["connected_clients"] = boost::lexical_cast<std::string>(total);

    return result;
}

std::shared_ptr<server_t::pool_type>
server_t::pool(const std::string& app) {
    std::lock_guard<std::mutex> lock(mutex);

    auto it = services.find(app);

    // Create connection to the application if one doesn't exist. Synchronous at this moment.
    if (it == services.end()) {
        CP_DEBUG("processing application pool creation")("app", app, "size", pool_size);

        it = services.insert(std::make_pair(
            app,
            std::make_shared<pool_type>(
                pool_size,
                reconnect_timeout,
                service_manager,
                request_timeout,
                logger(),
                app
            )
        )).first;
    }

    return it->second;
}
