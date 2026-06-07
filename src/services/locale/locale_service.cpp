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
#include <unicode/locid.h>
#include "../../utils/dbus_utils.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Locale
{

    LocaleService::LocaleService(ServiceManager* serviceManager,
                                 Connection* conn) :
                                 Service(serviceManager, conn),
                                 _object(*_conn, GetBaseObjectPath()),
                                 _iface(_object.CreateInterface(GetBaseInterface())),
                                 _proxy(*_conn,
                                     "org.freedesktop.locale1",
                                     ObjectPath("/org/freedesktop/locale1"),
                                     InterfaceName("org.freedesktop.locale1"))
    {
        _iface.RegisterProperty<std::string>(
            JOSLC_PROP_LOCALE,
            [&]{ return GetLocale(); },
            [&](const auto& locale) { SetLocale(locale); }
        );

        _iface.RegisterMethod("GetLocales", [&](const auto& m) { OnGetLocales(m); });

        DiscoverLocales();

        _subLocaleChanged = std::make_unique<SignalSubscription>(
            _proxy.SubscribePropertyChanged<std::vector<std::string>>(
                "Locale",
                [&](const std::vector<std::string>& val)
                {
                    const auto locale = ReadFreedesktopLocale(val);
                    _currentLocale = locale;
                    EmitLocaleChanged(locale);
                }
            )
        );
    }

    LocaleService::~LocaleService() = default;

    const std::map<std::string, std::string>& LocaleService::ListLocales() { return _locales; }

    std::string LocaleService::GetLocale()
    {
        if (!_currentLocale.empty())
            return _currentLocale;

        const auto locales = _proxy.GetProperty<std::vector<std::string>>("Locale");
        _currentLocale = ReadFreedesktopLocale(locales);
        return _currentLocale;
    }

    void LocaleService::SetLocale(const std::string& locale) const
    {
        const std::vector<std::string> localeSettings{
            "LANG=" + locale
        };

        _proxy.CallMethodNoReply("SetLocale", localeSettings, false);
    }

    void LocaleService::OnGetLocales(const Message& message) const
    {
        auto msg = Message::CreateMethodReturn(message);
        msg.SetArgs(_locales);
        msg.Send(*_conn);
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
                discovered[locale] = GetDisplayName(locale);
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

    std::string LocaleService::GetDisplayName(const std::string& locale)
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