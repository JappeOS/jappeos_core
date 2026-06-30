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
#include <fstream>
#include <optional>
#include <unordered_map>

#include "installer_def.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer
{
    class InitBootFile
    {
    public:
        InitBootFile() = delete;

        InitBootFile(const InitBootFile&) = delete;
        InitBootFile(InitBootFile&&) noexcept = default;
        InitBootFile& operator=(const InitBootFile&) = delete;
        InitBootFile& operator=(InitBootFile&&) noexcept = default;

        ~InitBootFile()
        {
            try
            {
                Close();
            } catch (...) {}
        }

        void Close()
        {
            if (_writer.has_value())
            {
                _writer.value().close();
                _writer = std::nullopt;
            }

            if (_reader.has_value())
            {
                _reader.value().close();
                _reader = std::nullopt;
            }
        }

        void WriteLocale(const std::string& locale)                         { Write(KEY_LOCALE, locale); }
        void WriteTimezone(const std::string& timezone)                     { Write(KEY_TIMEZONE, timezone); }
        void WriteKeyboardLayout(const std::string& keyboardLayout)         { Write(KEY_KBL, keyboardLayout); }
        void WriteKeyboardLayoutVariant(const std::string& keyboardVariant) { Write(KEY_KBLV, keyboardVariant); }
        void WriteHostname(const std::string& hostname)                     { Write(KEY_HOST, hostname); }
        void WriteUsername(const std::string& username)                     { Write(KEY_USER, username); }
        void WritePassword(const std::string& password)                     { Write(KEY_PASS, password); }

        std::string ReadLocale()                const { return Read(KEY_LOCALE); }
        std::string ReadTimezone()              const { return Read(KEY_TIMEZONE); }
        std::string ReadKeyboardLayout()        const { return Read(KEY_KBL); }
        std::string ReadKeyboardLayoutVariant() const { return Read(KEY_KBLV); }
        std::string ReadHostname()              const { return Read(KEY_HOST); }
        std::string ReadUsername()              const { return Read(KEY_USER); }
        std::string ReadPassword()              const { return Read(KEY_PASS); }

    public:
        static constexpr std::string KEY_LOCALE   = "LOCALE";
        static constexpr std::string KEY_TIMEZONE = "TIMEZONE";
        static constexpr std::string KEY_KBL      = "KBL";
        static constexpr std::string KEY_KBLV     = "KBLV";
        static constexpr std::string KEY_HOST     = "HOST";
        static constexpr std::string KEY_USER     = "USER";
        static constexpr std::string KEY_PASS     = "PASS";

        static InitBootFile Writer(const std::filesystem::path& path = STORAGE_SYSTEM_FILE_INIT_BOOT_PATH) { return InitBootFile(path, true); }
        static InitBootFile Reader(const std::filesystem::path& path = STORAGE_SYSTEM_FILE_INIT_BOOT_PATH) { return InitBootFile(path, false); }

    private:
        std::optional<std::ofstream> _writer;
        std::optional<std::ifstream> _reader;

        std::unordered_map<std::string, std::string> _readData;

        explicit InitBootFile(const std::filesystem::path& path, const bool write)
        {
            if (write)
            {
                _writer.emplace();
                _writer->exceptions(std::ios::failbit | std::ios::badbit);
                _writer->open(path);
            }
            else
            {
                _reader.emplace();
                _reader->exceptions(std::ios::failbit | std::ios::badbit);
                _reader->open(path);
                ReadFull();
            }
        }

        void ReadFull()
        {
            if (!_reader.has_value())
                throw std::logic_error("Cannot read from InitBootFile: file is open for writing");
            _readData.clear();

            std::string line;
            while (std::getline(_reader.value(), line))
            {
                if (line.empty() || line[0] == '#')
                    continue;

                const size_t pos = line.find('=');
                if (pos == std::string::npos)
                    continue;

                std::string key = line.substr(0, pos);
                const std::string value = line.substr(pos + 1);

                _readData[key] = value;
            }
        }

        std::string Read(const std::string& key) const
        {
            if (!_reader.has_value())
                throw std::logic_error("Cannot read from InitBootFile: file is open for writing");

            const auto it = _readData.find(key);
            return it != _readData.end() ? it->second : "";
        }

        void Write(const std::string& key, const std::string &value)
        {
            if (!_writer.has_value())
                throw std::logic_error("Cannot write to InitBootFile: file is open for reading");
            if (value.find('\n') != std::string::npos)
                throw std::runtime_error("Cannot write to InitBootFile: value cannot contain newline");
            _writer.value() << key << "=" << value << std::endl;
        }
    };
}
