#pragma once

#include <cstddef>
#include <list>
#include <optional>
#include <unordered_map>
#include <utility>

namespace proxy::cache {

    // Least-recently-used map bounded by a total "cost" (bytes, for the proxy).
    //
    // get() and put() are O(1): a doubly linked list keeps recency order
    // (front = most recent) and a hash map points into the list.
    //
    // Not thread-safe: callers synchronize (HttpCache holds a mutex).
    template <typename Key, typename Value, typename Hash = std::hash<Key>>
    class LruCache {
    public:
        explicit LruCache(std::size_t capacity) : capacity_(capacity) {}

        // Returns the value and marks it as most recently used.
        std::optional<Value> get(const Key& key) {
            auto it = index_.find(key);
            if (it == index_.end()) return std::nullopt;
            items_.splice(items_.begin(), items_, it->second);
            return it->second->value;
        }

        // Returns the value without touching recency.
        std::optional<Value> peek(const Key& key) const {
            auto it = index_.find(key);
            if (it == index_.end()) return std::nullopt;
            return it->second->value;
        }

        // Inserts or replaces. Evicts least recently used items until it fits.
        // Returns false (and stores nothing) if `cost` alone exceeds the capacity.
        bool put(const Key& key, Value value, std::size_t cost) {
            if (cost > capacity_) {
                erase(key);
                return false;
            }
            auto it = index_.find(key);
            if (it != index_.end()) {
                total_cost_ -= it->second->cost;
                it->second->value = std::move(value);
                it->second->cost = cost;
                items_.splice(items_.begin(), items_, it->second);
            } else {
                items_.push_front(Node{key, std::move(value), cost});
                index_.emplace(key, items_.begin());
            }
            total_cost_ += cost;
            evict_to_fit();
            return true;
        }

        bool erase(const Key& key) {
            auto it = index_.find(key);
            if (it == index_.end()) return false;
            total_cost_ -= it->second->cost;
            items_.erase(it->second);
            index_.erase(it);
            return true;
        }

        bool contains(const Key& key) const { return index_.count(key) != 0; }

        void clear() {
            items_.clear();
            index_.clear();
            total_cost_ = 0;
        }

        std::size_t size() const { return index_.size(); }
        std::size_t total_cost() const { return total_cost_; }
        std::size_t capacity() const { return capacity_; }
        std::size_t evictions() const { return evictions_; }

    private:
        struct Node {
            Key key;
            Value value;
            std::size_t cost;
        };

        void evict_to_fit() {
            while (total_cost_ > capacity_ && !items_.empty()) {
                auto& victim = items_.back();
                total_cost_ -= victim.cost;
                index_.erase(victim.key);
                items_.pop_back();
                ++evictions_;
            }
        }

        std::size_t capacity_;
        std::size_t total_cost_ = 0;
        std::size_t evictions_ = 0;
        std::list<Node> items_;
        std::unordered_map<Key, typename std::list<Node>::iterator, Hash> index_;
    };

}  // namespace proxy::cache