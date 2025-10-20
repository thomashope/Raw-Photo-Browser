#pragma once

#include <queue>
#include <mutex>
#include <optional>

template <typename T>
class ConcurrentQueue {
public:
    // Push an item onto the queue
    void push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(value));
    }
    
    // Try to pop an item from the queue
    // Returns true and sets 'out' if successful, false if queue is empty
    bool tryPop(T& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }
    
    // Check if queue is empty (note: result may be stale immediately after return)
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }
    
    // Get queue size (note: result may be stale immediately after return)
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;  // mutable to allow locking in const methods
};
