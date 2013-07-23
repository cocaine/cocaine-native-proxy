#ifndef SERVICE_POOL_HPP
#define SERVICE_POOL_HPP

#include <cocaine/framework/service.hpp>

#include <algorithm>

namespace cocaine { namespace proxy {

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
                 const Args&... args) :
        m_reconnect_timeout(reconnect_timeout),
        m_manager(manager),
        m_service_constructor(std::bind(&service_pool::construct<Args...>, this, args...)),
        m_next(0)
    {
        m_connections.reserve(size);
        m_next_reconnects.reserve(size);

        for (size_t i = 0; i < size; ++i) {
            m_connections.push_back(manager->get_service<Service>(args...));
        }

        time_t now = time(0);
        for (size_t i = 0; i < size; ++i) {
            m_next_reconnects.push_back(now + m_reconnect_timeout + rand() % m_reconnect_timeout);
        }
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

    service_wrapper<Service>
    operator->();

private:
    template<class... Args>
    std::shared_ptr<Service>
    construct(const Args&... args) {
        auto manager = m_manager.lock();
        return manager->get_service<Service>(args...);
    }

private:
    unsigned int m_reconnect_timeout;
    std::weak_ptr<cocaine::framework::service_manager_t> m_manager;
    std::function<std::shared_ptr<Service>()> m_service_constructor;
    std::vector<service_wrapper<Service>> m_connections;
    std::vector<time_t> m_next_reconnects;
    size_t m_next;
    std::mutex m_connections_lock;
};

template<class Service>
service_wrapper<Service>
service_pool<Service>::operator->() {
    std::unique_lock<std::mutex> lock(m_connections_lock);

    size_t old_next = m_next;

    time_t now = time(0);
    size_t reconnected = 0;
    size_t max_reconnects = std::min(static_cast<size_t>(1), m_connections.size() / 3);
    try {
        do {
            if (m_next_reconnects[m_next] < now) {
                m_connections[m_next] = service_wrapper<Service>(m_service_constructor());
                m_next_reconnects[m_next] = now + m_reconnect_timeout + rand() % m_reconnect_timeout;
                ++reconnected;
            }
            m_next = (m_next + 1) % m_connections.size();
        } while (m_next != old_next && reconnected < max_reconnects);
    } catch (...) {
        // pass
    }

    m_next = (old_next + 1) % m_connections.size();
    return m_connections[old_next];
}

}} // namespace cocaine::proxy

#endif // SERVICE_POOL_HPP

