#pragma once
#include "../service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Installer
{
    class InstallerService : public Service
    {
    public:
        explicit InstallerService(ServiceManager* serviceManager);
        ~InstallerService() override;

        bool HandleMethodCall(DBusMessage* msg) override;
        std::string GetName() override { return "InstallerService"; }

    private:
        bool BeginInstallation();
    };
}
