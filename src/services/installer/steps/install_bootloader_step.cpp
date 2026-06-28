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

#include "install_bootloader_step.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../../utils/command_runner.h"

using JappeStudios::JappeOS::JappeOSCore::Utils::CommandRunner;

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer::Steps
{

    void InstallBootloaderStep::Execute(InstallContext& context)
    {
        if (context.targetRoot.empty())
            throw std::runtime_error("Cannot install bootloader with empty target root path");

        if (context.rootPartition.empty())
            throw std::runtime_error("Cannot install bootloader with empty root partition");

        const std::filesystem::path targetRoot(context.targetRoot);
        const std::filesystem::path bootRoot = targetRoot / "boot";

        if (!std::filesystem::is_directory(bootRoot))
            throw std::runtime_error("Cannot install bootloader because target /boot is missing");

        const auto kernel = FindFirstExisting(
            bootRoot,
            {
                "vmlinuz-linux",
                "vmlinuz-jappeos",
                "vmlinuz",
            },
            "kernel image"
        );

        const auto initramfs = FindFirstExisting(
            bootRoot,
            {
                "initramfs-linux.img",
                "initramfs-jappeos.img",
                "initramfs.img",
            },
            "initramfs image"
        );

        WriteMkinitcpioConfig(targetRoot);

        CommandRunner::RunOrThrow({
            "arch-chroot",
            context.targetRoot,
            "mkinitcpio",
            "-P",
        });

        CommandRunner::RunOrThrow({
            "arch-chroot",
            context.targetRoot,
            "bootctl",
            "--esp-path=/boot",
            "install",
        });

        WriteLoaderConfig(bootRoot);
        WriteBootEntry(context, targetRoot, bootRoot, kernel, initramfs);
    }

    std::string InstallBootloaderStep::ToBootLoaderPath(const std::filesystem::path& bootRoot,
                                                        const std::filesystem::path& path)
    {
        const auto relative = std::filesystem::relative(path, bootRoot);
        return "/" + relative.generic_string();
    }

    std::filesystem::path InstallBootloaderStep::FindFirstExisting(const std::filesystem::path& bootRoot,
                                                                   const std::vector<std::filesystem::path>& candidates,
                                                                   const std::string& description)
    {
        for (const auto& candidate : candidates)
        {
            const auto path = bootRoot / candidate;
            if (std::filesystem::is_regular_file(path))
                return path;
        }

        throw std::runtime_error("Could not find " + description + " in target /boot");
    }

    std::string InstallBootloaderStep::ReadCommandOutput(const std::vector<std::string>& command, const std::string& description)
    {
        auto result = CommandRunner::Run(command);
        if (result.exitCode != 0)
            throw JappeStudios::JappeOS::JappeOSCore::Utils::CommandFailedException(
                command,
                result.exitCode,
                result.stdErr.empty() ? "<empty>" : result.stdErr
            );

        while (!result.stdOut.empty() &&
               (result.stdOut.back() == '\n' || result.stdOut.back() == '\r' || result.stdOut.back() == ' '))
        {
            result.stdOut.pop_back();
        }

        if (result.stdOut.empty())
            throw std::runtime_error("Could not determine " + description);

        return result.stdOut;
    }

    std::string InstallBootloaderStep::KernelOptions(const InstallContext& context)
    {
        const std::string partUuid = ReadCommandOutput(
            {"blkid", "-s", "PARTUUID", "-o", "value", context.rootPartition},
            "root partition PARTUUID"
        );

        return "root=PARTUUID=" + partUuid + " rw " + KERNEL_PARAMS;
    }

    void InstallBootloaderStep::WriteMkinitcpioConfig(const std::filesystem::path& systemRoot)
    {
        const auto configDir = systemRoot / "etc" / "mkinitcpio.conf.d";
        const auto presetDir = systemRoot / "etc" / "mkinitcpio.d";

        for (const auto& entry : std::filesystem::directory_iterator(configDir))
            std::filesystem::remove_all(entry.path());

        for (const auto& entry : std::filesystem::directory_iterator(presetDir))
            std::filesystem::remove_all(entry.path());

        std::filesystem::create_directories(configDir);
        std::filesystem::create_directories(presetDir);

        std::ofstream config(configDir / "jappeos.conf", std::ios::trunc);
        if (!config)
            throw std::runtime_error("Failed to write mkinitcpio jappeos.conf");

        config
            << "HOOKS=(" << MKINITCPIO_HOOKS << ")\n"
            << "COMPRESSION=\"xz\"\n"
            << "COMPRESSION_OPTIONS=(-9e)";

        std::ofstream preset(presetDir / "linux.preset", std::ios::trunc);
        if (!preset)
            throw std::runtime_error("Failed to write mkinitcpio linux.preset");

        preset
            << "# mkinitcpio preset file for the 'linux' package\n"
            << "PRESETS=('jappeos' 'fallback')\n"
            << "ALL_config='/etc/mkinitcpio.conf'\n"
            << "ALL_kver='/boot/vmlinuz-linux'\n"
            << "default_image='/boot/initramfs-linux.img'\n"
            << "fallback_image='/boot/initramfs-linux-fallback.img'\n"
            << "fallback_options='-S autodetect'";
    }

    void InstallBootloaderStep::WriteLoaderConfig(const std::filesystem::path& bootRoot)
    {
        const auto loaderDir = bootRoot / "loader";
        std::filesystem::create_directories(loaderDir);

        std::ofstream config(loaderDir / "loader.conf", std::ios::trunc);
        if (!config)
            throw std::runtime_error("Failed to write systemd-boot loader.conf");

        config
            << "default jappeos.conf\n"
            << "timeout 3\n"
            << "console-mode max\n"
            << "editor no\n";
    }

    void InstallBootloaderStep::WriteBootEntry(const InstallContext& context,
                                               const std::filesystem::path& systemRoot,
                                               const std::filesystem::path& bootRoot,
                                               const std::filesystem::path& kernel,
                                               const std::filesystem::path& initramfs)
    {
        const auto entriesDir = bootRoot / "loader" / "entries";
        std::filesystem::create_directories(entriesDir);
        const auto kernelOptions = KernelOptions(context);

        std::ofstream entry(entriesDir / "jappeos.conf", std::ios::trunc);
        if (!entry)
            throw std::runtime_error("Failed to write systemd-boot entry");

        entry
            << "title JappeOS\n"
            << "linux " << ToBootLoaderPath(bootRoot, kernel) << "\n"
            << "initrd " << ToBootLoaderPath(bootRoot, initramfs) << "\n"
            << "options " << kernelOptions << "\n";

        std::ofstream cmdline(systemRoot / "etc" / "cmdline.d" / "root.conf", std::ios::trunc);
        if (!cmdline)
            throw std::runtime_error("Failed to write boot options");

        cmdline << kernelOptions;
    }

}
