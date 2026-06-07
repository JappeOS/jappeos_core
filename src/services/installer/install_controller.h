/*
 * jappeos_core, Core system management daemon for JappeOS.
 * Copyright (C) 2026  The JappeOS team.
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
#include <thread>

#include "install_data.h"
#include "install_state.h"
#include "install_step.h"
#include "../../logger.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer
{
    struct InstallControllerCallbacks
    {
        std::function<void(InstallState)> stateChanged;
        std::function<void(const InstallProgress&)> progressChanged;
    };

    class InstallController
    {
    public:
        explicit InstallController(InstallControllerCallbacks callbacks,
                                   std::vector<std::unique_ptr<InstallStep>> steps);
        ~InstallController();
        void StartInstall(const InstallData& data);
        void CancelInstall();

        [[nodiscard]] InstallState State() const { return _state.load(); }

    private:
        std::jthread _worker;
        const InstallControllerCallbacks _callbacks;
        const std::vector<std::unique_ptr<InstallStep>> _steps;
        std::atomic<InstallState> _state{InstallState::Idle};
        std::atomic_bool _cancelRequested;

        void RunInstall(const InstallData& data);
        void UpdateState(InstallState state);
        void UpdateProgress(std::string step, uint32_t percent, std::string message) const;

    private:
        static JappeOSCore::Logger& Log()
        {
            static JappeOSCore::Logger instance{"InstallController"};
            return instance;
        }
    };
}
