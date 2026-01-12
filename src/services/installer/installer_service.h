#pragma once
#include "../service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer
{
    class InstallerService : public Service
    {
    public:
        explicit InstallerService(ServiceManager* serviceManager);
        ~InstallerService() override;

        bool HandleMethodCallLegacy(DBusMessage* msg) override;
        [[nodiscard]] std::string GetName() const override { return "InstallerService"; }

    private:
        bool BeginInstallation();
    };
}
