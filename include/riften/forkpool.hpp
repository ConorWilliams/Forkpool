
#pragma once

#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <optional>
#include <semaphore>
#include <thread>

#include "riften/deque.hpp"
#include "riften/detail/eventcount.hpp"
#include "riften/detail/xoshiro.hpp"

namespace riften {

static thread_local std::size_t static_id;

class Forkpool {
  private:
    struct task_handle : std::coroutine_handle<> {
        std::uint64_t* alpha;
    };

    using task_t = std::optional<task_handle>;

  public:
    static void schedule(task_handle handle) {
        get()._deque[static_id].emplace(handle);

        if (static_id == get()._thread.size()) {
            get()._notifyer.notify_one();
        }
    }

    static task_t pop() noexcept { return get()._deque[static_id].pop(); }

  private:
    static Forkpool& get() {
        static Forkpool pool{std::thread::hardware_concurrency()};
        return pool;
    }

    explicit Forkpool(std::size_t n = std::thread::hardware_concurrency()) : _deque(n + 1) {
        // Master thread uses nth deque
        static_id = n;

        for (std::size_t id = 0; id < n; ++id) {
            _thread.emplace_back([&, id] {
                // Set id for calls to fork
                static_id = id;

                // Initialise PRNG stream
                for (size_t j = 0; j < id; j++) {
                    detail::long_jump();
                }

                task_t task = std::nullopt;

                while (true) {
                    exploit_task(id, task);
                    if (wait_for_task(id, task) == false) {
                        break;
                    }
                }
            });
        }
    }

    ~Forkpool() {
        _stop.store(true);
        _notifyer.notify_all();
    }

    void exploit_task(std::size_t id, task_t& task) {
        if (task) {
            if (_actives.fetch_add(1, std::memory_order_acq_rel) == 0
                && _thieves.load(std::memory_order_acquire) == 0) {
                _notifyer.notify_one();
            }

            task->resume();

            assert(!_deque[id].pop());

            _actives.fetch_sub(1, std::memory_order_release);
        }
    }

    void steal_task(std::size_t id, task_t& task) {
        for (std::size_t i = 0; i < _thread.size(); ++i) {
            if (auto v = detail::xrand() % _thread.size(); v == id) {
                task = _deque.back().steal();
            } else {
                task = _deque[v].steal();
            }
            if (task) {
                assert(task->alpha);
                *(task->alpha) += 1;
                return;
            }
        }
    }

    bool wait_for_task(std::size_t id, task_t& task) {
    wait_for_task:
        _thieves.fetch_add(1, std::memory_order_release);
    steal_task:
        if (steal_task(id, task); task) {
            if (_thieves.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                _notifyer.notify_one();
            }
            return true;
        }

        auto key = _notifyer.prepare_wait();

        if (!_deque.back().empty()) {
            _notifyer.cancel_wait();
            task = _deque.back().steal();
            if (task) {
                if (_thieves.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    _notifyer.notify_one();
                }
                return true;
            } else {
                goto steal_task;
            }
        }

        if (_stop.load()) {
            _notifyer.cancel_wait();
            _notifyer.notify_all();
            _thieves.fetch_sub(1, std::memory_order_release);
            return false;
        }

        if (_thieves.fetch_sub(1, std::memory_order_acq_rel) == 1
            && _actives.load(std::memory_order_acquire) > 0) {
            _notifyer.cancel_wait();
            goto wait_for_task;
        }

        // std::osyncstream(std::cout) << static_id << " sleeps\n";

        _notifyer.wait(key);

        return true;
    }

  private:
    alignas(hardware_destructive_interference_size) std::atomic<std::int64_t> _actives = 0;
    alignas(hardware_destructive_interference_size) std::atomic<std::int64_t> _thieves = 0;
    alignas(hardware_destructive_interference_size) std::atomic<bool> _stop = false;

    detail::event_count _notifyer;

    std::vector<Deque<task_handle>> _deque;
    std::vector<std::jthread> _thread;
};

}  // namespace riften