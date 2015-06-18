#pragma once

#include <memory>

#include <boost/variant/variant.hpp>

namespace cocaine { namespace proxy {

template<class Service>
class soft_killer {
    std::shared_ptr<Service> service;

public:
    soft_killer(std::shared_ptr<Service> service):
        service(std::move(service))
    {}

    ~soft_killer() {
        if (service) {
            try {
                service->soft_destroy();
            } catch (...) {
                // pass
            }
        }
    }

    std::shared_ptr<Service>
    get() const {
        return service;
    }

    void
    release() {
        service.reset();
    }
};

template<class Service>
class service_wrapper {
    std::shared_ptr<soft_killer<Service>> killer;

public:
    service_wrapper() {}

    service_wrapper(std::shared_ptr<Service> service) :
        killer(std::make_shared<soft_killer<Service>>(std::move(service)))
    {}

    operator bool() noexcept {
        return killer.get() != nullptr;
    }

    std::shared_ptr<Service>
    operator->() {
        return killer->get();
    }

    std::shared_ptr<Service const>
    operator->() const {
        return killer->get();
    }

    void
    release() {
        killer->release();
    }
};

template<class T>
class shared_future {
    typedef framework::promise<T> promise_type;
    typedef framework::future<T>  future_type;

    enum class state_t {
        pending,
        ready_ok,
        ready_err
    };

    state_t state;

    T value;
    std::exception err;

    std::deque<promise_type> queue;
    std::mutex mutex;

public:
    shared_future():
        state(state_t::pending)
    {}

    void
    notify_all(T value) {
        std::unique_lock<std::mutex> lock(mutex);

        state = state_t::ready_ok;
        if (queue.empty()) {
            this->value = std::move(value);
        } else {
            auto queue = std::move(this->queue);
            lock.unlock();

            for (auto& promise : queue) {
                promise.set_value(value);
            }
        }
    }

    void
    notify_all(std::exception err) {
        std::unique_lock<std::mutex> lock(mutex);

        state = state_t::ready_err;
        if (queue.empty()) {
            this->err = std::move(err);
        } else {
            auto queue = std::move(this->queue);
            lock.unlock();

            for (auto& promise : queue) {
                promise.set_exception(err);
            }
        }
    }

    future_type
    get() {
        std::unique_lock<std::mutex> lock(mutex);

        switch (state) {
        case state_t::ready_ok:
            return framework::make_ready_future<T>::value(this->value);
        case state_t::ready_err:
            return framework::make_ready_future<T>::error(this->err);
        default:
            break;
        }

        promise_type pr;
        auto fr = pr.get_future();
        queue.push_back(std::move(pr));
        return fr;
    }
};

}} // namespace cocaine::proxy
