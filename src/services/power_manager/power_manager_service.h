#pragma once
#include "../service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::PowerManager
{
    class PowerManagerService : public Service
    {
    public:
        explicit PowerManagerService(ServiceManager* serviceManager, Connection* conn);
        ~PowerManagerService() override;
        [[nodiscard]] std::string GetName() const override { return "PowerManagerService"; }

    private:
        Object _mainObject;

        void SharedPolicy(const Message& message) const;

        void OnShutdown(const Message& message) const;
        void OnReboot(const Message& message) const;
        void OnSuspend(const Message& message) const;
    };
}
