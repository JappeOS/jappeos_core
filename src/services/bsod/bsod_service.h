#pragma once

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/vt.h>
#include <linux/kd.h>
#include <termios.h>
#include <csignal>

#include "../service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Bsod
{
    // TODO: Prevent closing VT
    class BsodService : public Service
    {
    public:
        explicit BsodService(ServiceManager* serviceManager, DBusConnection* conn);
        //~BsodService() override;

        bool HandleMethodCall(DBusMessage* msg, const std::string& subInterface) override { return false; }
        std::string GetName() override { return "BsodService"; }

        /// This method displays an error message and requires the computer to reboot. This should only be called during
        /// an irrecoverable error condition. It is not recommended to use this from other services.
        bool ShowBSODDangerousSync(const std::string& message);

    private:
        // Helper: write full buffer, retry on EINTR, handle partial writes.
        static bool write_all(int fd, const char* buf, size_t len) {
            if (fd < 0) return false;
            size_t off = 0;
            while (off < len) {
                ssize_t n = write(fd, buf + off, len - off);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    // EAGAIN would mean non-blocking and would need poll/select; treat as failure here.
                    return false;
                }
                off += static_cast<size_t>(n);
            }
            return true;
        }
        static bool write_all(int fd, const std::string& s) {
            return write_all(fd, s.data(), s.size());
        }

    private:
        int _ttyfd = -1;
        int _vtfd = -1;

        bool OpenAndActivateVT();
        bool RegisterSignals();
        [[nodiscard]] bool Paint(const std::string& message) const;
        void WaitForSignalOrTimeout() const;
    };
}
