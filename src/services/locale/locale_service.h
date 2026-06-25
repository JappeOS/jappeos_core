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
#include "../service.h"
#include "../../event.h"
#include "../../logger.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Locale
{
#define JOSLC_PROP_LOCALE   "Locale"
#define JOSLC_PROP_TIMEZONE "Timezone"

    class LocaleService : public Service
    {
    public:
        Event<const std::string&> OnLocaleChanged;
        Event<const std::string&> OnTimezoneChanged;

        explicit LocaleService(ServiceManager* serviceManager, Connection* conn);
        ~LocaleService() override;
        [[nodiscard]] std::string GetName() const override { return "LocaleService"; }

        const std::map<std::string, std::string>& ListLocales();
        std::vector<std::string> ListTimezones() const;
        std::string GetLocale();
        std::string GetTimezone() const;
        void SetLocale(const std::string& locale) const;
        void SetTimezone(const std::string& timezone, bool interactive = false) const;

    private:
        Object     _object;
        Interface& _iface;
        Proxy      _proxyLocale1;
        Proxy      _proxyTimedate1;
        std::unique_ptr<SignalSubscription> _subLocaleChanged;
        std::unique_ptr<SignalSubscription> _subTimezoneChanged;

        std::map<std::string, std::string> _locales;
        std::string _currentLocale;

        void OnGetLocales(const Message& message) const;
        void OnSetLocale(const Message& message) const;
        void OnGetTimezones(const Message& message) const;
        void OnSetTimezone(const Message& message) const;
        void EmitLocaleChanged(const std::string& locale) const;
        void EmitTimezoneChanged(const std::string& timezone) const;

        void DiscoverLocales();

    private:
        static JappeOSCore::Logger& Log()
        {
            static JappeOSCore::Logger instance{"LocaleService"};
            return instance;
        }

        static std::string GetLocaleDisplayName(const std::string& locale);
        static std::string ReadFreedesktopLocale(const std::vector<std::string>& locales);
    };
}