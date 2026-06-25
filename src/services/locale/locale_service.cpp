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

#include "locale_service.h"

#include <filesystem>
#include <algorithm>
#include <unicode/locid.h>
#include "../../utils/dbus_utils.h"

namespace fs = std::filesystem;

namespace JappeStudios::JappeOS::JappeOSCore::Services::Locale
{

    LocaleService::LocaleService(ServiceManager* serviceManager,
                                 Connection* conn) :
                                 Service(serviceManager, conn),
                                 _object(*_conn, GetBaseObjectPath()),
                                 _iface(_object.CreateInterface(GetBaseInterface())),
                                 _proxyLocale1(*_conn,
                                     "org.freedesktop.locale1",
                                     ObjectPath("/org/freedesktop/locale1"),
                                     InterfaceName("org.freedesktop.locale1")),
                                 _proxyTimedate1(*_conn,
                                     "org.freedesktop.timedate1",
                                     ObjectPath("/org/freedesktop/timedate1"),
                                     InterfaceName("org.freedesktop.timedate1"))
    {
        _iface.RegisterProperty<std::string>(
            JOSLC_PROP_LOCALE, [&]{ return GetLocale(); }
        );

        _iface.RegisterProperty<std::string>(
            JOSLC_PROP_TIMEZONE, [&]{ return GetTimezone(); }
        );

        _iface.RegisterMethod("GetLocales", [&](const auto& m) { OnGetLocales(m); });
        _iface.RegisterMethod("SetLocale", [&](const auto& m) { OnSetLocale(m); });
        _iface.RegisterMethod("GetTimezones", [&](const auto& m) { OnGetTimezones(m); });
        _iface.RegisterMethod("SetTimezone", [&](const auto& m) { OnSetTimezone(m); });

        DiscoverLocales();

        _subLocaleChanged = std::make_unique<SignalSubscription>(
            _proxyLocale1.SubscribePropertyChanged<std::vector<std::string>>(
                "Locale",
                [&](const std::vector<std::string>& val)
                {
                    const auto locale = ReadFreedesktopLocale(val);
                    _currentLocale = locale;
                    EmitLocaleChanged(locale);
                }
            )
        );

        _subTimezoneChanged = std::make_unique<SignalSubscription>(
            _proxyTimedate1.SubscribePropertyChanged<std::string>(
                "Timezone",
                [&](const std::string& val) { EmitTimezoneChanged(val); }
            )
        );
    }

    LocaleService::~LocaleService() = default;

    const std::map<std::string, std::string>& LocaleService::ListLocales() { return _locales; }

    std::vector<std::string> LocaleService::ListTimezones() const
    {
        const auto [timezones] = _proxyTimedate1.CallMethod<std::vector<std::string>>("ListTimezones");
        return timezones;
    }

    std::string LocaleService::GetLocale()
    {
        if (!_currentLocale.empty())
            return _currentLocale;

        const auto locales = _proxyLocale1.GetProperty<std::vector<std::string>>("Locale");
        _currentLocale = ReadFreedesktopLocale(locales);
        return _currentLocale;
    }

    std::string LocaleService::GetTimezone() const
    {
        const auto timezone = _proxyLocale1.GetProperty<std::string>("Timezone");
        return timezone;
    }

    void LocaleService::SetLocale(const std::string& locale) const
    {
        const std::vector<std::string> localeSettings{
            "LANG=" + locale
        };

        _proxyLocale1.CallMethodNoReply("SetLocale", localeSettings, false);
    }

    void LocaleService::SetTimezone(const std::string& timezone, const bool interactive) const
    {
        _proxyTimedate1.CallMethodNoReply("SetTimezone", timezone, interactive);
    }

    void LocaleService::OnGetLocales(const Message& message) const
    {
        auto msg = Message::CreateMethodReturn(message);
        msg.SetArgs(_locales);
        msg.Send(*_conn);
    }

    void LocaleService::OnSetLocale(const Message& message) const
    {
        const auto [locale, interactive] = message.GetArgs<std::string, bool>();
        // TODO: Implement auth and interactive
        SetLocale(locale);
    }

    void LocaleService::OnGetTimezones(const Message& message) const
    {
        auto msg = Message::CreateMethodReturn(message);
        msg.SetArgs(ListTimezones());
        msg.Send(*_conn);
    }

    void LocaleService::OnSetTimezone(const Message& message) const
    {
        const auto [timezone, interactive] = message.GetArgs<std::string, bool>();
        SetTimezone(timezone, interactive);
    }

    void LocaleService::EmitLocaleChanged(const std::string& locale) const
    {
        try
        {
            OnLocaleChanged(locale);
        }
        catch (const std::exception& e)
        {
            Log().Err("OnLocaleChanged event handler failed with an error: " + std::string(e.what()));
        }
        Utils::DBusUtils::EmitPropertyChanged(*_conn, _iface, JOSLC_PROP_LOCALE, locale);
    }

    void LocaleService::EmitTimezoneChanged(const std::string& timezone) const
    {
        try
        {
            OnTimezoneChanged(timezone);
        }
        catch (const std::exception& e)
        {
            Log().Err("OnTimezoneChanged event handler failed with an error: " + std::string(e.what()));
        }
        Utils::DBusUtils::EmitPropertyChanged(*_conn, _iface, JOSLC_PROP_TIMEZONE, timezone);
    }

    void LocaleService::DiscoverLocales()
    {
        std::map<std::string, std::string> discovered;

        FILE* pipe = popen("locale -a", "r");
        if (!pipe)
            throw std::runtime_error("Failed to open pipe for 'locale -a': " + std::string(strerror(errno)));

        char buffer[256];

        while (fgets(buffer, sizeof(buffer), pipe))
        {
            std::string locale(buffer);

            while (!locale.empty() &&
                   (locale.back() == '\n' || locale.back() == '\r'))
            {
                locale.pop_back();
            }

            if (locale.empty())
                continue;

            try
            {
                discovered[locale] = GetLocaleDisplayName(locale);
            }
            catch (const std::exception&)
            {
                // Skip malformed/unsupported locales without aborting the whole discovery
                continue;
            }
        }

        if (ferror(pipe))
        {
            pclose(pipe);
            throw std::runtime_error("Error reading from 'locale -a' pipe: " + std::string(strerror(errno)));
        }

        pclose(pipe);
        _locales = std::move(discovered);  // Only commit on full success
    }

    std::string LocaleService::GetLocaleDisplayName(const std::string& locale)
    {
        const icu::Locale loc = icu::Locale::createCanonical(locale.c_str());

        if (loc.isBogus())
        {
            return locale;
        }

        icu::UnicodeString displayName;
        loc.getDisplayName(displayName);

        std::string converted;
        displayName.toUTF8String(converted);

        if (converted.empty() || converted == loc.getName())
        {
            converted = locale;
        }

        return converted;
    }

    std::string LocaleService::ReadFreedesktopLocale(const std::vector<std::string>& locales)
    {
        for (const auto& entry : locales)
        {
            constexpr auto prefix = "LANG=";

            if (entry.rfind(prefix, 0) == 0)
                return entry.substr(strlen(prefix));
        }

        throw std::runtime_error("LANG not found in locale data received from org.freedesktop.locale1");
    }

}