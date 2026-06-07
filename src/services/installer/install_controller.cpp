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

#include "install_controller.h"

#include <utility>
#include <glib/gmain.h>

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer
{

    InstallController::InstallController(InstallControllerCallbacks callbacks,
                                         std::vector<std::unique_ptr<InstallStep>> steps) :
                                         _callbacks(std::move(callbacks)),
                                         _steps(std::move(steps))
    {
    }

    InstallController::~InstallController()
    {
        if (_state.load() == InstallState::Running)
        {
            Log().Warn("InstallController::~InstallController - Install still in progress! Trying to cancel.");
            CancelInstall();
        }

        if (_worker.joinable())
            _worker.join();
    }

    void InstallController::StartInstall(const InstallData& data)
    {
        if (_state.load() == InstallState::Running)
            throw std::logic_error("StartInstall called, but install is already in progress!");

        UpdateState(InstallState::Running);

        _worker = std::jthread(
            [this, data]
            {
                RunInstall(data);
            }
        );

        _worker.detach();
    }

    void InstallController::CancelInstall()
    {
        if (_state.load() == InstallState::Running)
        {
            Log().Info("Install cancel request received.");
            _cancelRequested = true;
        }
    }

    void InstallController::RunInstall(const InstallData& data)
    {
        _cancelRequested = false;
        Log().Info("Install started!");

        auto context = InstallContext{data};
        for (int i = 0; i < _steps.size(); ++i)
        {
            if (_cancelRequested)
            {
                Log().Warn("Install cancelled by user!");
                UpdateState(InstallState::Cancelled);
                return;
            }

            const auto& step = _steps[i];
            const auto stepName = step->Name();
            const auto progress = (100 / _steps.size()) * i;
            UpdateProgress(
                stepName,
                progress,
                "Installing..."
            );

            Log().Info("Installing... step='" + stepName + "', progress='" + std::to_string(progress) + "%'");

            try
            {
                step->Execute(context);
            }
            catch (const std::exception& e)
            {
                Log().Crit("Install failed: " + std::string(e.what()));
                UpdateState(InstallState::Failed);
                return;
            }
        }

        UpdateProgress(
            "Done",
            100,
            "Install done!"
        );

        Log().Info("Install succeeded!");
        UpdateState(InstallState::Succeeded);
    }

    void InstallController::UpdateState(const InstallState state)
    {
        _state.store(state);

        struct UserData
        {
            const InstallController& installController;
            InstallState installState;
        };

        g_main_context_invoke(
            nullptr,
            [](const gpointer data)
            {
                const auto* userdata = static_cast<UserData*>(data);
                userdata->installController._callbacks.stateChanged(userdata->installState);
                delete userdata;
                return G_SOURCE_REMOVE;
            },
            new UserData{
                *this,
                state,
            }
        );
    }

    void InstallController::UpdateProgress(std::string step, const uint32_t percent, std::string message) const
    {
        struct UserData
        {
            const InstallController& installController;
            InstallProgress installProgress;
        };

        g_main_context_invoke(
            nullptr,
            [](const gpointer data)
            {
                const auto* userdata = static_cast<UserData*>(data);
                userdata->installController._callbacks.progressChanged(userdata->installProgress);
                delete userdata;
                return G_SOURCE_REMOVE;
            },
            new UserData{
                *this,
                InstallProgress{
                    std::move(step),
                    percent,
                    std::move(message)
                }
            }
        );
    }

}
