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

#ifndef SERVICE_POOL_HPP
#define SERVICE_POOL_HPP

#include <cocaine/framework/service.hpp>

#include <algorithm>

namespace cocaine { namespace proxy {

template<class T>
class shared_future {
    typedef cocaine::framework::promise<T> promise_type;
    typedef cocaine::framework::future<T>  future_type;

    int ready;
    T v;
    std::exception err;

    std::deque<promise_type> queue;
    std::mutex mutex;

public:
    shared_future() : ready(0) {}

    void
    notify_all(T v) {
        std::unique_lock<std::mutex> lock(mutex);

        ready = 1;
        if (queue.empty()) {
            this->v = v;
        } else {
            auto copy = std::move(queue);
            lock.unlock();

            for (auto& q : copy) {
                q.set_value(v);
            }
        }
    }

    void
    notify_all(std::exception err) {
        std::unique_lock<std::mutex> lock(mutex);

        ready = 2;
        if (queue.empty()) {
            this->err = err;
        } else {
            auto copy = std::move(queue);
            lock.unlock();

            for (auto& q : copy) {
                q.set_exception(err);
            }
        }
    }

    future_type
    get() {
        std::unique_lock<std::mutex> lock(mutex);

        if (ready == 1) {
            return cocaine::framework::make_ready_future<T>::value(this->v);
        } else if (ready == 2) {
            return cocaine::framework::make_ready_future<T>::error(this->err);
        } else {
            promise_type pr;
            auto f = pr.get_future();
            queue.push_back(std::move(pr));
            return f;
        }
    }
};

template<class Service>
struct soft_killer {
    soft_killer(std::shared_ptr<Service> service) :
        m_service(service)
    {
        // pass
    }

    ~soft_killer() {
        if (m_service) {
            try {
                m_service->soft_destroy();
            } catch (...) {
                // pass
            }
        }
    }

    std::shared_ptr<Service>
    get() const {
        return m_service;
    }

    void
    release() {
        m_service.reset();
    }

private:
    std::shared_ptr<Service> m_service;
};

template<class Service>
struct service_wrapper {
    service_wrapper() {
        // pass
    }

    operator bool() {
        return m_killer.get() != nullptr;
    }

    service_wrapper(std::shared_ptr<Service> service) :
        m_killer(new soft_killer<Service>(service))
    {
        // pass
    }

    service_wrapper(const service_wrapper& other) :
        m_killer(other.m_killer)
    {
        // pass
    }

    std::shared_ptr<Service>
    operator->() {
        return m_killer->get();
    }

    std::shared_ptr<Service const>
    operator->() const {
        return m_killer->get();
    }

    void
    release() {
        m_killer->release();
    }

private:
    std::shared_ptr<soft_killer<Service>> m_killer;
};

template<class Service>
struct service_pool {
    service_pool() {
        // pass
    }

    template<class... Args>
    service_pool(size_t size,
                 unsigned int reconnect_timeout,
                 std::shared_ptr<cocaine::framework::service_manager_t> manager,
                 float request_timeout,
                 const Args&... args) :
        m_reconnect_timeout(reconnect_timeout),
        m_manager(manager),
        m_service_constructor(std::bind(&service_pool::construct<Args...>, this, request_timeout, args...)),
        m_service_async_constructor(std::bind(&service_pool::construct_async<Args...>, this, request_timeout, args...)),
        m_next(0)
    {
        m_connections.reserve(size);
        m_next_reconnects.reserve(size);

        for (size_t i = 0; i < size; ++i) {
            m_connections.push_back(service_wrapper<Service>(m_service_constructor()));
        }

        time_t now = time(0);
        for (size_t i = 0; i < size; ++i) {
            m_next_reconnects.push_back(now + m_reconnect_timeout + rand() % m_reconnect_timeout);
        }

        m_futures.resize(size);
    }

    service_pool(service_pool&& other) :
        m_reconnect_timeout(other.m_reconnect_timeout),
        m_manager(std::move(other.m_manager)),
        m_service_constructor(std::move(other.m_service_constructor)),
        m_connections(std::move(other.m_connections)),
        m_next_reconnects(std::move(other.m_next_reconnects)),
        m_next(other.m_next)
    {
        // pass
    }

    ~service_pool() {
        for (size_t i = 0; i < m_connections.size(); ++i) {
            m_connections[i].release();
        }
    }

    cocaine::framework::future<service_wrapper<Service>>
    get();

    size_t
    connected_clients() const {
        size_t result = 0;

        std::unique_lock<std::mutex> lock(m_connections_lock);
        for (auto it = m_connections.begin(); it != m_connections.end(); ++it) {
            if ((*it)->status() == cocaine::framework::service_status::connected) {
                ++result;
            }
        }

        return result;
    }

private:
    template<class... Args>
    std::shared_ptr<Service>
    construct(float timeout, const Args&... args) {
        auto manager = m_manager.lock();
        auto s = manager->get_service<Service>(args...);
        s->set_timeout(timeout);
        return s;
    }

    template<class... Args>
    framework::future<std::shared_ptr<Service>>
    construct_async(float timeout, const Args&... args) {
        auto manager = m_manager.lock();
        auto future = manager->get_service_async<Service>(args...);
        return future.then([&](framework::future<std::shared_ptr<Service>>& f) -> std::shared_ptr<Service> {
            auto s = f.get();
            s->set_timeout(timeout);
            return s;
        });
    }

private:
    unsigned int m_reconnect_timeout;
    std::weak_ptr<cocaine::framework::service_manager_t> m_manager;
    std::function<std::shared_ptr<Service>()> m_service_constructor;
    std::function<cocaine::framework::future<std::shared_ptr<Service>>()> m_service_async_constructor;
    std::vector<service_wrapper<Service>> m_connections;
    std::vector<std::shared_ptr<shared_future<service_wrapper<Service>>>> m_futures;
    std::vector<time_t> m_next_reconnects;
    size_t m_next;
    mutable std::mutex m_connections_lock;
};

template<class Service>
cocaine::framework::future<service_wrapper<Service>>
service_pool<Service>::get() {
    // reconnect after timeout
    try {
        time_t now = time(0);
        size_t reconnected = 0;
        size_t max_reconnects = std::min(static_cast<size_t>(1), m_connections.size() / 3);

        size_t old_next = m_next;
        size_t it = old_next;

        do {
            std::unique_lock<std::mutex> lock(m_connections_lock);
            if (m_next_reconnects[it] < now) {
                m_next_reconnects[it] = now + m_reconnect_timeout + rand() % m_reconnect_timeout;
                m_connections[it] = service_wrapper<Service>();
                m_futures[it] = std::make_shared<shared_future<service_wrapper<Service>>>();
                lock.unlock();

                m_service_async_constructor().then([=](cocaine::framework::future<std::shared_ptr<Service>>& f) {
                    // WARNING: Reference unsafe.
                    std::unique_lock<std::mutex> lock(m_connections_lock);
                    try {
                        m_connections[it] = f.get();
                        m_futures[it]->notify_all(m_connections[it]);
                    } catch (const std::exception& err) {
                        // Leave empty. Force to reconnect on next request.
                        m_next_reconnects[it] = 0;
                        m_futures[it]->notify_all(err);
                    }
                });
                ++reconnected;
            }
            it = (it + 1) % m_connections.size();
        } while (it != old_next && reconnected < max_reconnects);
    } catch (...) {
        // pass
    }

    // select connection to send request
    std::unique_lock<std::mutex> lock(m_connections_lock);

    size_t old_next = m_next;

    // select first connected service
    do {
        service_wrapper<Service> c = m_connections[m_next];
        m_next = (m_next + 1) % m_connections.size();
        if (c && c->status() == cocaine::framework::service_status::connected) {
            return cocaine::framework::make_ready_future<service_wrapper<Service>>::value(c);
        }
    } while (m_next != old_next);

    m_next = (old_next + 1) % m_connections.size();

    // not found.
    return m_futures[old_next]->get();
}

}} // namespace cocaine::proxy

#endif // SERVICE_POOL_HPP

