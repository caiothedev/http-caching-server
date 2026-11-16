#pragma once

#include <chrono>
#include <functional>
#include <memory>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>

namespace proxy {

    // Inactivity watchdog built on one steady_timer.
    //
    // Instead of cancelling and re-arming the timer on every I/O completion, the
    // owner just moves the deadline forward with extend(). When the timer fires it
    // compares the clock with the current deadline and either reports expiry or
    // waits again.
    //
    // The timer state lives in a shared block captured by the completion handler,
    // so the owner may be destroyed while a cancelled wait is still queued.
    // Must be used from a single strand.
    class Deadline {
        public:
            using clock = std::chrono::steady_clock;

            explicit Deadline(const boost::asio::any_io_executor& ex) : state_(std::make_shared<State>(ex)) {}
            // The owner's destructor may run on any thread (whichever drops the last
            // shared_ptr), concurrently with a cancelled wait finishing on the strand.
            // So only touch shared state if stop() never ran: while the deadline is
            // active, `on_expire` keeps the owner alive, so it cannot be destroyed then.
            ~Deadline() {
                if (!state_->stopped) stop();
            }

            Deadline(const Deadline&) = delete;
            Deadline& operator=(const Deadline&) = delete;

            // `on_expire` usually captures a shared_ptr to the owner; it is released on stop().
            void start(clock::duration timeout, std::function<void()> on_expire) {
                state_->on_expire = std::move(on_expire);
                state_->deadline = clock::now() + timeout;
                wait(state_);
            }

            void extend(clock::duration timeout) {
                auto next = clock::now() + timeout;
                bool earlier = next < state_->deadline;
                state_->deadline = next;
                if (earlier) state_->timer.cancel();  // wake up and re-wait on the closer deadline
            }

            // Suspends expiry (e.g. while another component enforces its own timeout).
            void suspend() { state_->deadline = clock::now() + std::chrono::hours(24 * 365); }

            void stop() {
                state_->stopped = true;
                state_->on_expire = nullptr;
                state_->timer.cancel();
            }

        private:
            struct State {
                explicit State(const boost::asio::any_io_executor& ex) : timer(ex) {}
                boost::asio::steady_timer timer;
                clock::time_point deadline = clock::now() + std::chrono::hours(24 * 365);
                std::function<void()> on_expire;
                bool stopped = false;
            };

            static void wait(const std::shared_ptr<State>& state) {
                state->timer.expires_at(state->deadline);
                state->timer.async_wait([state](const boost::system::error_code&) {
                    if (state->stopped) return;
                    if (clock::now() >= state->deadline) {
                        state->stopped = true;
                        auto cb = std::move(state->on_expire);
                        state->on_expire = nullptr;
                        if (cb) cb();
                        return;
                    }
                    wait(state);
                });
            }

        std::shared_ptr<State> state_;
    };

}  // namespace proxy