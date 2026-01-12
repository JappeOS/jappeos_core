#define GENERATE_ENUM_STRINGS
#include "application.h"

#include "services/account_manager/account_manager_service.h"
#include "services/session_manager/session_manager_service.h"
#include "services/watchdog/watchdog_service.h"
#include "services/bsod/bsod_service.h"
#include "services/power_manager/power_manager_service.h"
#include "utils/jos_exception.h"

namespace JappeStudios::JappeOS::JappeOSCore
{

    const char* ApplicationState_ToString(const ApplicationState state) {
        switch (state) {
#define X(name) case ApplicationState::name: return #name;
#include "application_state.def"
#undef X
            default: return "Unknown";
        }
    }

    Application* Application::s_instance = nullptr;
    volatile std::sig_atomic_t Application::s_signalCaught = 0;
    int Application::s_pipeFds[2] = { -1, -1 };

    uint8_t Application::Run()
    {
        _exitCode = EXIT_SUCCESS;
        try
        {
            _state = ApplicationState::Initializing;
            Initialize();
            _state = ApplicationState::Running;
            while (_shouldRun)
            {
                Update();

                if (s_signalCaught)
                {
                    switch (s_signalCaught)
                    {
                        case SIGABRT: throw Utils::JosException("SIGABRT", "");
                        case SIGBUS: throw Utils::JosException("SIGBUS", "");
                        case SIGFPE: throw Utils::JosException("SIGFPE", "");
                        case SIGHUP: throw Utils::JosException("SIGHUP", "");
                        case SIGILL: throw Utils::JosException("SIGILL", "");
                        case SIGPIPE: throw Utils::JosException("SIGPIPE", "");
                        case SIGQUIT: throw Utils::JosException("SIGQUIT", "");
                        case SIGSEGV: throw Utils::JosException("SIGSEGV", "");
                        case SIGSYS: throw Utils::JosException("SIGSYS", "");
                        case SIGUSR1: throw Utils::JosException("SIGUSR1", "");
                        case SIGUSR2: throw Utils::JosException("SIGUSR2", "");
                        case SIGXCPU: throw Utils::JosException("SIGXCPU", "");
                        case SIGXFSZ: throw Utils::JosException("SIGXFSZ", "");
                        default: Quit();
                    }
                }
            }
            _state = ApplicationState::Stopping;
            CleanUp();
            _state = ApplicationState::Stopped;
        }
        catch (const std::exception& e)
        {
            const auto logger = NULL_SAFE_CALL_RET(_serviceManager, Get<Services::Logger::LoggerService>());
            NULL_SAFE_CALL(logger, Alert(""));
            NULL_SAFE_CALL(logger, Alert("An unhandled exception was thrown! Printing exception information below ..."));
            NULL_SAFE_CALL(logger, Alert("+-- BEGIN EXCEPTION INFO --+"));
            NULL_SAFE_CALL(logger, Alert(""));
            NULL_SAFE_CALL(logger, Alert(std::string("Application State: ") + ApplicationState_ToString(_state)));
            NULL_SAFE_CALL(logger, Alert(""));
            NULL_SAFE_CALL(logger, Alert(std::string("Exception: ") + e.what()));
            NULL_SAFE_CALL(logger, Alert(""));
            NULL_SAFE_CALL(logger, Alert("+-- END EXCEPTION INFO --+"));
            NULL_SAFE_CALL(logger, Alert(""));

            if (_state == ApplicationState::Running)
            {
                NULL_SAFE_CALL(logger, Info("Application was running. Attempting cleanup."));
                try
                {
                    _state = ApplicationState::Stopping;
                    CleanUp();
                }
                catch (const std::exception&)
                {
                    NULL_SAFE_CALL(logger, Err("Cleanup failed."));
                }
            }

            _state = ApplicationState::Stopped;
            _exitCode = EXIT_FAILURE;

            std::string errorCode = "EXCEPTION_UNHANDLED";
            std::string stackTrace;
            if (auto* se = dynamic_cast<const std::system_error*>(&e))
            {
                errorCode = std::to_string(se->code().value());
            }
            else if (auto* je = dynamic_cast<const Utils::JosException*>(&e))
            {
                errorCode = je->ShortErrorCode();
                stackTrace = je->Stacktrace();
            }

            FailFastFatalError(
                errorCode,
                e.what(),
                stackTrace.empty() ? PrintStackTrace() : stackTrace
            );
        }

        return _exitCode;
    }

    void Application::Initialize()
    {
        if (pipe(s_pipeFds) == -1)
        {
            throw std::runtime_error("Failed to create signal pipe");
        }

        struct sigaction sa{};
        sa.sa_handler = SignalHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        // TODO
        //sigaction(SIGABRT, &sa, nullptr);
        //sigaction(SIGBUS, &sa, nullptr);
        //sigaction(SIGFPE, &sa, nullptr);
        //sigaction(SIGHUP, &sa, nullptr);
        //sigaction(SIGILL, &sa, nullptr);
        //sigaction(SIGPIPE, &sa, nullptr);
        //sigaction(SIGQUIT, &sa, nullptr);
        //sigaction(SIGSEGV, &sa, nullptr);
        //sigaction(SIGSYS, &sa, nullptr);
        //sigaction(SIGUSR1, &sa, nullptr);
        //sigaction(SIGUSR2, &sa, nullptr);
        //sigaction(SIGXCPU, &sa, nullptr);
        //sigaction(SIGXFSZ, &sa, nullptr);
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);

        _serviceManager = new Services::ServiceManager();
        InitDBus();
        _serviceManager->InitDBus(_conn);

        const auto logger = _serviceManager->Register<Services::Logger::StdoutLogger>();
        NULL_SAFE_CALL(logger, Debug("Initializing..."));
        InitServices();
        NULL_SAFE_CALL(logger, Debug("Initialization done!"));
    }

    void Application::InitDBus()
    {
        dbus_error_init(_err);
        _conn = new Connection(DBUS_BUS_SYSTEM);

        if (dbus_error_is_set(_err))
        {
            const std::string message = _err->message;
            dbus_error_free(_err);
            throw std::runtime_error("D-Bus error: " + message);
        }

        _conn->AcquireName(Services::DBUS_INTERFACE, DBUS_NAME_FLAG_DO_NOT_QUEUE);
        _conn->AddMatch("type='method_call'"); // TODO: Don't allow all method calls
        _conn->Flush();
        const int dbus_fd = _conn->GetUnixFd();

        _fds[0].fd = dbus_fd;
        _fds[0].events = POLLIN;

        _fds[1].fd = s_pipeFds[0];
        _fds[1].events = POLLIN;
    }

    void Application::InitServices() const
    {
        _serviceManager->Register<Services::Bsod::BsodService>();
        _serviceManager->Register<Services::Watchdog::WatchdogService>();
        _serviceManager->Register<Services::SessionManager::SessionManagerService>();
        _serviceManager->Register<Services::AccountManager::AccountManagerService>();
        _serviceManager->Register<Services::PowerManager::PowerManagerService>();
    }

    void Application::Update()
    {
        if (poll(_fds, 2, -1) < 0) return;

        static const auto logger = NULL_SAFE_CALL_RET(_serviceManager, Get<Services::Logger::LoggerService>());

        if (_fds[1].revents & POLLIN)
        {
            char buf[64];
            read(s_pipeFds[0], buf, sizeof(buf));

            NULL_SAFE_CALL(logger, Info("Received signal, exiting..."));
            Quit();
            return;
        }

        if (_fds[0].revents & POLLIN)
        {
            while (_conn->ReadWrite(0))
            {
                auto msg = _conn->PopMessage();
                if (!msg.has_value() || _serviceManager == nullptr) break;

                OnMessageHandlerPre();
                HandleDBusMessageLegacy(logger, msg.value().GetRawMessage());
                _conn->HandleMessage(msg.value());
                //_serviceManager->HandleMessage(msg.value());
                OnMessageHandlerPost();
            }
        }
    }

    void Application::HandleDBusMessageLegacy(Services::Logger::LoggerService* logger, DBusMessage* msg) const
    {
        const char* interface = dbus_message_get_interface(msg);
        if (!interface)
            return;

        auto services = _serviceManager->ListNamed();

        if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL)
        {
            const std::string key = std::string(interface) + "." + dbus_message_get_member(msg);
            const auto& signalSubscribers = _serviceManager->ListSignalSubscribers();
            if (const auto subscribers = signalSubscribers.find(key); subscribers != signalSubscribers.end())
            {
                for (const auto svc : subscribers->second)
                {
                    HandleMethodCallLegacy(logger, svc, msg, interface);
                }
            }
            return;
        }

        if (const auto it = services.find(interface); it != services.end())
        {
            HandleMethodCallLegacy(logger, it->second, msg, interface);
        }
    }

    void Application::HandleMethodCallLegacy(Services::Logger::LoggerService* logger, Services::Service* svc, DBusMessage* msg, const char* interface) const
    {
        if (!msg)
        {
            NULL_SAFE_CALL(logger, Err(std::string("HandleMethodCall called with null parameters for service `" + svc->GetName() + "` with interface: ") + interface));;
            return;
        }

        try
        {
            if (!svc->HandleMethodCallLegacy(msg))
            {
                NULL_SAFE_CALL(logger, Debug(std::string("Method not handled in service `" + svc->GetName() + "` with interface: ") + interface));
            }
        }
        catch (const std::exception& e)
        {
            NULL_SAFE_CALL(logger, Crit(std::string("(non-fatal) Unhandled exception in service `" + svc->GetName() + "` while handling method call: ") + e.what()));
        }
    }

    void Application::CleanUp() const
    {
        const auto logger = NULL_SAFE_CALL_RET(_serviceManager, Get<Services::Logger::LoggerService>());
        NULL_SAFE_CALL(logger, Debug("Cleaning up..."));
        dbus_error_free(_err);
        delete _err;
        _serviceManager->RunBeforeCleanup(logger, [&]
        {
            NULL_SAFE_CALL(logger, Debug("Cleanup done!"));
        });
        delete _serviceManager;
        delete _conn;
    }

    void Application::FailFastFatalError(const std::string& errCode, const std::string& message, const std::string& stack) const
    {
        const auto bsod = NULL_SAFE_CALL_RET(_serviceManager, Get<Services::Bsod::BsodService>());
        const auto logger = NULL_SAFE_CALL_RET(_serviceManager, Get<Services::Logger::LoggerService>());

        auto newErrCode = errCode;
        for (auto& c: newErrCode) c = toupper(c);
        std::erase(newErrCode, ' ');

        const auto newStack = stack.empty() ? PrintStackTrace() : stack;

        NULL_SAFE_CALL(logger, Emerg("[Application::FailFastFatalError] FailFastFatalError called! ErrCode: " + newErrCode + ", Message: " + message + ", Stack: " + newStack + ". This indicates that a fatal system error has occurred."));
        NULL_SAFE_CALL(logger, Info("[Application::FailFastFatalError] Trying to display BSOD."));

        if (bsod == nullptr)
        {
            NULL_SAFE_CALL(logger, Err("[Application::FailFastFatalError] Failed to display BSOD! Service is not initialized!"));
            return;
        }

        bsod->ShowBSODDangerousSync(
            "A fatal error has occurred and the system needs to be reset!"
            + (message.empty() ? "\n\n" + message : "Please check the system logs for more information.")
            + (stack.empty() ? "\n\nStack: \n" + stack : "")
            + (newErrCode.empty() ? "\n\nError: " + newErrCode : "ERROR_UNKNOWN")
        );

        NULL_SAFE_CALL(logger, Crit("[Application::FailFastFatalError] Exiting immediately because of unrecoverable fatal error."));
        exit(EXIT_FAILURE);
    }

    std::string Application::PrintStackTrace(unsigned int max_frames)
    {
        std::ostringstream oss;

        // Storage array for stack trace address data
        void* addrlist[max_frames + 1];

        // Retrieve current stack addresses
        int addrlen = backtrace(addrlist, sizeof(addrlist) / sizeof(void*));
        if (addrlen == 0)
        {
            oss << "<empty, possibly corrupt>\n";
            return oss.str();
        }

        // Resolve addresses into strings containing "filename(function+address)"
        char** symbols = backtrace_symbols(addrlist, addrlen);
        if (!symbols)
        {
            oss << "<backtrace_symbols failed>\n";
            return oss.str();
        }

        // Iterate over returned symbol lines
        for (int i = 0; i < addrlen; i++)
        {
            std::string sym(symbols[i]);
            std::string function_name = sym;

            // Try to extract the mangled name between '(' and '+'
            const size_t begin = sym.find('(');
            size_t end   = sym.find('+', begin);
            if (begin != std::string::npos && end != std::string::npos)
            {
                std::string mangled = sym.substr(begin + 1, end - begin - 1);
                int status = 0;
                char* demangled = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
                if (status == 0 && demangled)
                {
                    function_name = sym.substr(0, begin + 1) + demangled + sym.substr(end);
                    std::free(demangled);
                }
            }

            oss << "[" << i << "] " << function_name << "\n";
        }

        free(symbols);
        return oss.str();
    }

}
