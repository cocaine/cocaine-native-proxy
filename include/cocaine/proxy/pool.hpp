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

#pragma once

#include <algorithm>

#include <swarm/logger.hpp>

#include <cocaine/framework/service.hpp>

#include "cocaine/proxy/common.hpp"
#include "cocaine/proxy/util.hpp"

namespace cocaine { namespace proxy {

template<class Service>
class reconnectable_service {
public:
    typedef service_wrapper<Service> service_type;
    typedef std::function<framework::future<std::shared_ptr<Service>>()> ctor_type;

private:
    service_type service;
    ctor_type ctor;

    uint timeout;
    std::time_t deadline;

    std::shared_ptr<shared_future<service_type>> future;

    mutable std::mutex mutex;

public:
    reconnectable_service(service_type service, ctor_type ctor, uint timeout):
        service(std::move(service)),
        ctor(std::move(ctor)),
        timeout(timeout),
        deadline(0)
    {}

    bool
    expired() const {
        std::unique_lock<std::mutex> lock(mutex);
        return std::time(nullptr) > deadline;
    }

    bool
    connecting() const {
        std::unique_lock<std::mutex> lock(mutex);
        return connecting(lock);
    }

    bool
    connected() const {
        std::unique_lock<std::mutex> lock(mutex);
        return connected(lock);
    }

    framework::future<service_type>
    get() {
        std::unique_lock<std::mutex> lock(mutex);
        if (connected(lock)) {
            return framework::make_ready_future<service_type>::value(service);
        }

        return reconnect(lock);
    }

    framework::future<service_type>
    reconnect() {
        std::unique_lock<std::mutex> lock(mutex);
        return reconnect(lock);
    }

private:
    bool
    connecting(std::unique_lock<std::mutex>&) const {
        return future.get() != nullptr;
    }

    bool
    connected(std::unique_lock<std::mutex>&) const {
        return service->status() == framework::service_status::connected;
    }

    framework::future<service_type>
    reconnect(std::unique_lock<std::mutex>& lock) {
        if (connecting(lock)) {
            return future->get();
        }

        future = std::make_shared<shared_future<service_type>>();
        auto result = future->get();
        lock.unlock();

        ctor().then([=](framework::future<std::shared_ptr<Service>>& f) {
            const auto now = std::time(nullptr);

            std::unique_lock<std::mutex> lock(mutex);
            try {
                service = f.get();
                deadline = now + timeout + rand() % timeout;
                future->notify_all(service);
            } catch (const std::exception& err) {
                BOOST_ASSERT(future);
                future->notify_all(err);
            }

            future.reset();
        });

        return result;
    }
};

template<class Service>
class service_pool {
    const swarm::logger& log;

    /// Reconnect timeout.
    unsigned int timeout;

    /// Service manager.
    std::weak_ptr<framework::service_manager_t> manager;

    std::function<std::shared_ptr<Service>()> service_ctor;
    std::function<framework::future<std::shared_ptr<Service>>()> service_async_ctor;

    std::vector<std::shared_ptr<reconnectable_service<Service>>> services;

    /// Current connection cursor.
    std::size_t pos;

    mutable std::mutex mutex;

public:
    service_pool() {}

    template<class... Args>
    service_pool(std::size_t size,
                 unsigned int timeout,
                 std::shared_ptr<framework::service_manager_t> manager,
                 float request_timeout,
                 const swarm::logger& log,
                 const Args&... args) :
        log(log),
        timeout(timeout),
        manager(std::move(manager)),
        service_ctor(std::bind(&service_pool::construct<Args...>, this, request_timeout, args...)),
        service_async_ctor(std::bind(&service_pool::construct_async<Args...>, this, request_timeout, args...)),
        pos(0)
    {
        services.reserve(size);
        for (std::size_t i = 0; i < size; ++i) {
            services.push_back(
                std::make_shared<reconnectable_service<Service>>(
                    service_wrapper<Service>(service_ctor()),
                    service_async_ctor,
                    timeout
                )
            );
        }
    }

    ~service_pool() {
    }

    const swarm::logger&
    logger() const noexcept {
        return log;
    }

    std::size_t
    clients() const {
        std::size_t result = 0;

        std::lock_guard<std::mutex> lock(mutex);
        for (auto& service : services) {
            if (service->connected()) {
                ++result;
            }
        }

        return result;
    }

    framework::future<service_wrapper<Service>>
    next() {
        const std::size_t max_inprogress = std::min(static_cast<std::size_t>(1), services.size() / 3);

        std::unique_lock<std::mutex> lock(mutex);
        for (auto& service : services) {
            if (!service->connected()) {
                service->reconnect();
            }
        }

        std::size_t inprogress = 0;
        for (const auto& service : services) {
            if (service->connecting()) {
                inprogress++;
            }
        }

        for (auto& service : services) {
            if (inprogress >= max_inprogress) {
                break;
            }

            if (service->expired()) {
                service->reconnect();
                ++inprogress;
            }
        }

        const std::size_t prev = pos;
        do {
            auto& service = services[pos];
            pos = (pos + 1) % services.size();
            if (service->connected()) {
                CP_DEBUG("returning connected service")("id", pos);
                return service->get();
            }
        } while (pos != prev);

        pos = (prev + 1) % services.size();
        CP_DEBUG("returning future")("id", pos);

        return services[pos]->get();
    }

private:
    template<class... Args>
    std::shared_ptr<Service>
    construct(float timeout, const Args&... args) {
        auto manager = this->manager.lock();
        auto service = manager->template get_service<Service>(args...);
        service->set_timeout(timeout);

        return service;
    }

    template<class... Args>
    framework::future<std::shared_ptr<Service>>
    construct_async(float timeout, const Args&... args) {
        auto manager = this->manager.lock();
        auto future = manager->template get_service_async<Service>(args...);

        return future.then([&](framework::future<std::shared_ptr<Service>>& f) -> std::shared_ptr<Service> {
            auto service = f.get();
            service->set_timeout(timeout);
            return service;
        });
    }
};

}} // namespace cocaine::proxy
