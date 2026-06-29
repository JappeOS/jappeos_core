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
#include <string_view>
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

        const auto kernel = FindKernel(bootRoot);
        const auto kernelName = KernelName(kernel);

        WriteMkinitcpioConfig(targetRoot, kernel);

        CommandRunner::RunOrThrow({
            "arch-chroot",
            context.targetRoot,
            "mkinitcpio",
            "-P",
        });

        const auto initramfs = FindFirstExisting(
            bootRoot,
            {
                InitramfsImageFileName(kernelName),
                InitramfsImageFileName("linux"),
                InitramfsImageFileName("jappeos"),
                "initramfs.img",
            },
            "initramfs image"
        );

        const auto initramfsFallback = FindFirstExisting(
            bootRoot,
            {
                InitramfsImageFileName(kernelName, true),
                InitramfsImageFileName("linux", true),
                InitramfsImageFileName("jappeos", true),
                "initramfs-fallback.img",
            },
            "initramfs fallback image"
        );

        CommandRunner::RunOrThrow({
            "arch-chroot",
            context.targetRoot,
            "bootctl",
            "--esp-path=/boot",
            "install",
        });

        WriteLoaderConfig(bootRoot);
        WriteBootEntry(context, targetRoot, bootRoot, kernel, initramfs, initramfsFallback);
    }

    std::string InstallBootloaderStep::ToBootLoaderPath(const std::filesystem::path& bootRoot,
                                                        const std::filesystem::path& path)
    {
        const auto relative = std::filesystem::relative(path, bootRoot);
        return "/" + relative.generic_string();
    }

    std::string InstallBootloaderStep::ToChrootPath(const std::filesystem::path& systemRoot,
                                                    const std::filesystem::path& path)
    {
        const auto relative = std::filesystem::relative(path, systemRoot);
        return "/" + relative.generic_string();
    }

    std::filesystem::path InstallBootloaderStep::FindKernel(const std::filesystem::path& bootRoot)
    {
        std::vector<std::filesystem::path> kernels;
        for (const auto& entry : std::filesystem::directory_iterator(bootRoot))
        {
            if (!entry.is_regular_file())
                continue;

            const auto filename = entry.path().filename().string();
            if (filename.rfind("vmlinuz-", 0) == 0 || filename == "vmlinuz")
                kernels.push_back(entry.path());
        }

        if (kernels.empty())
            throw std::runtime_error("Could not find kernel image in target /boot");

        for (const auto& kernel : kernels)
            if (kernel.filename() == "vmlinuz-linux")
                return kernel;

        return kernels.front();
    }

    std::string InstallBootloaderStep::KernelName(const std::filesystem::path& kernel)
    {
        const auto filename = kernel.filename().string();
        constexpr std::string_view prefix = "vmlinuz-";
        if (filename.rfind(prefix, 0) == 0)
            return filename.substr(prefix.size());

        if (filename == "vmlinuz")
            return "linux";

        throw std::runtime_error("Unsupported kernel filename: " + filename);
    }

    std::filesystem::path InstallBootloaderStep::InitramfsImagePath(const std::filesystem::path& bootRoot,
                                                                    const std::string& kernelName,
                                                                    const bool isFallback)
    {
        return bootRoot / InitramfsImageFileName(kernelName, isFallback);
    }

    std::string InstallBootloaderStep::InitramfsImageFileName(const std::string& kernelName,
                                                              const bool isFallback)
    {
        return "initramfs-" + kernelName + (isFallback ? "-fallback" : "") + ".img";
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

    void InstallBootloaderStep::WriteMkinitcpioConfig(const std::filesystem::path& systemRoot,
                                                      const std::filesystem::path& kernel)
    {
        const auto configDir = systemRoot / "etc" / "mkinitcpio.conf.d";
        const auto presetDir = systemRoot / "etc" / "mkinitcpio.d";
        const auto mainConfigPath = systemRoot / "etc" / "mkinitcpio.conf";

        std::filesystem::remove_all(configDir);
        std::filesystem::remove_all(presetDir);
        std::filesystem::create_directories(configDir);
        std::filesystem::create_directories(presetDir);

        std::ofstream mainConfig(mainConfigPath, std::ios::trunc);
        if (!mainConfig)
            throw std::runtime_error("Failed to write mkinitcpio.conf");

        mainConfig
            << "MODULES=()\n"
            << "BINARIES=()\n"
            << "FILES=()\n"
            << "HOOKS=()\n";

        std::ofstream config(configDir / "jappeos.conf", std::ios::trunc);
        if (!config)
            throw std::runtime_error("Failed to write mkinitcpio jappeos.conf");

        config
            << "HOOKS=(" << MKINITCPIO_HOOKS << ")\n";

        const auto kernelName = KernelName(kernel);
        const auto presetPath = presetDir / (kernelName + ".preset");

        std::ofstream preset(presetPath, std::ios::trunc);
        if (!preset)
            throw std::runtime_error("Failed to write mkinitcpio preset");

        preset
            << "# mkinitcpio preset file for the '" << kernelName << "' kernel\n"
            << "PRESETS=('default' 'fallback')\n"
            << "ALL_config='" << ToChrootPath(systemRoot, mainConfigPath) << "'\n"
            << "ALL_kver='" << ToChrootPath(systemRoot, kernel) << "'\n"
            << "default_image='" << ToChrootPath(systemRoot, InitramfsImagePath(systemRoot / "boot", kernelName)) << "'\n"
            << "fallback_image='" << ToChrootPath(systemRoot, InitramfsImagePath(systemRoot / "boot", kernelName, true)) << "'\n"
            << "fallback_options='-S autodetect'\n";
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
                                               const std::filesystem::path& initramfs,
                                               const std::filesystem::path& initramfsFallback)
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

        std::ofstream fallbackEntry(entriesDir / "jappeos-fallback.conf", std::ios::trunc);
        if (!fallbackEntry)
            throw std::runtime_error("Failed to write systemd-boot fallback entry");

        fallbackEntry
            << "title JappeOS (Fallback)\n"
            << "linux " << ToBootLoaderPath(bootRoot, kernel) << "\n"
            << "initrd " << ToBootLoaderPath(bootRoot, initramfsFallback) << "\n"
            << "options " << kernelOptions << "\n";

        const auto cmdlineDir = systemRoot / "etc" / "cmdline.d";
        std::filesystem::create_directories(cmdlineDir);

        std::ofstream cmdline(cmdlineDir / "root.conf", std::ios::trunc);
        if (!cmdline)
            throw std::runtime_error("Failed to write boot options");

        cmdline << kernelOptions;
    }

}
