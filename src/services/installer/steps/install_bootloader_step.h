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
#include <filesystem>

#include "../install_step.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer::Steps
{
    class InstallBootloaderStep : public InstallStep
    {
    public:
        [[nodiscard]] std::string Name() const override { return "InstallBootloaderStep"; }
        void Execute(InstallContext& context) override;

    private:
        static constexpr auto MKINITCPIO_HOOKS = "base udev plymouth autodetect microcode modconf kms keyboard keymap block filesystems fsck";
        static constexpr auto KERNEL_PARAMS = "quiet splash loglevel=3 rd.systemd.show_status=auto rd.udev.log_level=3 vt.global_cursor_default=0";

        static std::string ToBootLoaderPath(const std::filesystem::path& bootRoot,
                                            const std::filesystem::path& path);
        static std::filesystem::path FindFirstExisting(const std::filesystem::path& bootRoot,
                                                       const std::vector<std::filesystem::path>& candidates,
                                                       const std::string& description);
        static std::string ReadCommandOutput(const std::vector<std::string>& command, const std::string& description);
        static std::string KernelOptions(const InstallContext& context);
        static void WriteMkinitcpioConfig(const std::filesystem::path& systemRoot);
        static void WriteLoaderConfig(const std::filesystem::path& bootRoot);
        static void WriteBootEntry(const InstallContext& context,
                                   const std::filesystem::path& systemRoot,
                                   const std::filesystem::path& bootRoot,
                                   const std::filesystem::path& kernel,
                                   const std::filesystem::path& initramfs);
    };
}
