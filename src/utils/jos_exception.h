#pragma once
#include <exception>
#include <stdexcept>
#include <string>

#include "../application.h"

namespace JappeStudios::JappeOS::JappeOSCore::Utils
{
    class JosException final : std::runtime_error
    {
    public:
        explicit JosException(const std::string& shortErrorCode, const std::string& msg) : std::runtime_error(msg)
        {
            auto newErrCode = shortErrorCode;
            for (auto & c: newErrCode) c = toupper(c);
            std::erase(newErrCode, ' ');
            _shortErrorCode = newErrCode;

            _stacktrace = Application::PrintStackTrace();
        }

        [[nodiscard]] const std::string& ShortErrorCode() const noexcept { return _shortErrorCode; }
        [[nodiscard]] const std::string& Stacktrace() const noexcept { return _stacktrace; }

    private:
        std::string _shortErrorCode;
        std::string _stacktrace;
    };
}
