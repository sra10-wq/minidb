#pragma once
#include <cstddef>
#include <iterator>
#include <list>
#include <optional>
#include <unordered_map>

// Tracks frames that can be evicted (pin_count == 0), least recently used at the front.
// A list plus a map gives O(1) pin, unpin, and victim.
class LRUReplacer {
public:
    // Frame was released: it becomes evictable, as the most recently used.
    void unpin(int frame) {
        if (pos_.count(frame)) return;
        order_.push_back(frame);
        pos_[frame] = std::prev(order_.end());
    }
    // Frame is in use again: it is no longer evictable.
    void pin(int frame) {
        auto it = pos_.find(frame);
        if (it == pos_.end()) return;
        order_.erase(it->second);
        pos_.erase(it);
    }
    // Remove and return the least recently used evictable frame.
    std::optional<int> victim() {
        if (order_.empty()) return std::nullopt;
        int frame = order_.front();
        order_.pop_front();
        pos_.erase(frame);
        return frame;
    }
    std::size_t size() const { return order_.size(); }

private:
    std::list<int> order_;
    std::unordered_map<int, std::list<int>::iterator> pos_;
};
