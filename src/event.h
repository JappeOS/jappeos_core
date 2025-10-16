#pragma once
#include <functional>
#include <unordered_map>
#include <cstddef>   // for std::size_t

namespace JappeStudios::JappeOS::JappeOSCore
{
    // Generic event system
    template <typename... Args>
    class Event {
    public:
        using Handler = std::function<void(Args...)>;

        // Subscribe: returns an ID you can use to unsubscribe later
        std::size_t Subscribe(Handler handler)
        {
            auto id = _nextId++;
            _handlers[id] = std::move(handler);
            return id;
        }

        // Unsubscribe by ID
        void Unsubscribe(std::size_t id)
        {
            _handlers.erase(id);
        }

        // Trigger (raise) the event
        void operator()(Args... args) const
        {
            for (auto& [id, handler] : _handlers) {
                handler(args...);
            }
        }

        // Clear all handlers
        void Clear()
        {
            _handlers.clear();
        }

    private:
        std::unordered_map<std::size_t, Handler> _handlers;
        std::size_t _nextId = 0;
    };
}