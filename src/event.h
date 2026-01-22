/*
 * jappeos_core, Core system management daemon for JappeOS.
 * Copyright (C) 2026  Jappe02
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once
#include <functional>
#include <unordered_map>
#include <cstddef>

namespace JappeStudios::JappeOS::JappeOSCore
{
    template <typename... Args>
    class Event
    {
    public:
        using Handler = std::function<void(Args...)>;

        std::size_t Subscribe(Handler handler)
        {
            auto id = _nextId++;
            _handlers[id] = std::move(handler);
            return id;
        }

        void Unsubscribe(std::size_t id)
        {
            _handlers.erase(id);
        }

        void operator()(Args... args) const
        {
            for (auto& [id, handler] : _handlers)
            {
                handler(args...);
            }
        }

        void Clear()
        {
            _handlers.clear();
        }

    private:
        std::unordered_map<std::size_t, Handler> _handlers;
        std::size_t _nextId = 0;
    };
}