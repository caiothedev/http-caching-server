#include "proxy/upstream_pool.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <iterator>
#include <vector>

namespace proxy {

    using tcp = boost::asio::ip::tcp;

    namespace {

        // True if the idle connection is still open and has nothing unread.
        bool still_usable(int fd) {
            char byte;
            for (;;) {
                auto n = ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
                if (n < 0 && errno == EINTR) continue;
                return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
            }
        }

    }  // namespace

    UpstreamPool::UpstreamPool(std::size_t max_idle, std::chrono::seconds idle_timeout)
        : max_idle_(max_idle), idle_timeout_(idle_timeout) {}

    UpstreamPool::~UpstreamPool() {
        for (const auto& item : idle_) ::close(item.fd);
    }

    void UpstreamPool::purge_expired(clock::time_point now) {
        while (!idle_.empty() && now - idle_.front().since >= idle_timeout_) {
            ::close(idle_.front().fd);
            idle_.pop_front();
        }
    }

    std::optional<tcp::socket> UpstreamPool::acquire(const std::string& key,
                                                     const boost::asio::any_io_executor& ex) {
        if (!enabled()) return std::nullopt;
        for (;;) {
            Idle found{};
            {
                std::lock_guard<std::mutex> lock(mutex_);
                purge_expired(clock::now());
                // Newest first: the most recently used connection is the least
                // likely to have been closed by the origin.
                auto it = std::find_if(idle_.rbegin(), idle_.rend(),
                                       [&](const Idle& item) { return item.key == key; });
                if (it == idle_.rend()) return std::nullopt;
                found = *it;
                idle_.erase(std::next(it).base());
            }
            if (!still_usable(found.fd)) {
                ::close(found.fd);
                continue;
            }
            tcp::socket socket(ex);
            boost::system::error_code ec;
            socket.assign(found.v6 ? tcp::v6() : tcp::v4(), found.fd, ec);
            if (ec) {
                ::close(found.fd);
                continue;
            }
            reused_++;
            return socket;
        }
    }

    void UpstreamPool::release(const std::string& key, tcp::socket socket) {
        boost::system::error_code ec;
        if (!enabled() || !socket.is_open()) return;
        bool v6 = false;
        auto local = socket.local_endpoint(ec);
        if (!ec) v6 = local.address().is_v6();
        int fd = ec ? -1 : socket.release(ec);
        if (ec || fd < 0) {
            socket.close(ec);
            return;
        }

        std::vector<int> to_close;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto now = clock::now();
            purge_expired(now);
            while (idle_.size() >= max_idle_) {
                to_close.push_back(idle_.front().fd);
                idle_.pop_front();
            }
            idle_.push_back({key, fd, v6, now});
        }
        for (int old : to_close) ::close(old);
    }

    std::size_t UpstreamPool::idle() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return idle_.size();
    }

}  // namespace proxy