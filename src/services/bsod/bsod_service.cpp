#include "bsod_service.h"

#include "../logger/logger_service.h"

namespace JappeStudios::JappeOS::JappeOSCore::Services::Bsod
{

    BsodService::BsodService(ServiceManager* serviceManager, DBusConnection* conn, DBusError* err) : Service(serviceManager, conn, err) {}

    bool BsodService::ShowBSODDangerousSync(const std::string& message)
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();
        logger->Info("Trying to display BSOD.");

        _ttyfd = -1;
        _vtfd = -1;

        if (!OpenAndActivateVT())
        {
            logger->Err("Failed to activate VT.");
            return false;
        }

        // Make sure we are in text mode (not graphics)
        // KDSETMODE / KD_TEXT is a best-effort (works on many consoles)
        int kd_text = KD_TEXT;
        if (ioctl(_ttyfd, KDSETMODE, &kd_text) < 0)
        {
            // not fatal; continue using ANSI
        }

        /*if (!RegisterSignals())
        {
            logger->Err("Failed to register signals.");
            return false;
        }*/

        if (!Paint(message))
        {
            logger->Err("Failed to paint BSOD.");
            return false;
        }

        WaitForSignalOrTimeout();

        close(_ttyfd);
        close(_vtfd);
        return true;
    }

    bool BsodService::OpenAndActivateVT()
    {
        const auto logger = _serviceManager->Get<Logger::LoggerService>();

        // Open /dev/tty0 for vt ioctls (controls active vt)
        _vtfd = open("/dev/tty0", O_RDWR | O_NONBLOCK);
        if (_vtfd < 0)
        {
            logger->Err("open /dev/tty0");
            return false;
        }

        // 1) Try to get a free VT from the kernel
        int free_vt = -1;
        if (ioctl(_vtfd, VT_OPENQRY, &free_vt) < 0)
        {
            logger->Err("ioctl VT_OPENQRY (ignored)");
            free_vt = -1;
        }

        int targetVT = -1;
        if (free_vt > 0)
        {
            targetVT = free_vt; // best option: a fresh unused VT
            logger->Debug("VT_OPENQRY -> free vt " + std::to_string(targetVT));
        }
        else
        {
            // 2) fallback: get current active VT
            struct vt_stat vts;
            if (ioctl(_vtfd, VT_GETSTATE, &vts) == 0)
            {
                targetVT = vts.v_active;
                logger->Debug("VT_GETSTATE -> active vt " + std::to_string(targetVT));
            }
            else
            {
                logger->Err("ioctl VT_GETSTATE (ignored)");
            }
        }

        // 3) final fallback: use controlling tty if it is /dev/ttyN
        if (targetVT <= 0)
        {
            if (char *ctty = ttyname(STDIN_FILENO))
            {
                // look for "/dev/ttyN"
                if (strncmp(ctty, "/dev/tty", 8) == 0)
                {
                    int n = atoi(ctty + 8);
                    if (n > 0)
                    {
                        targetVT = n;
                        logger->Debug(std::string("using controlling tty ") + ctty + " -> vt " + std::to_string(targetVT));
                    }
                }
                else
                {
                    logger->Err(std::string("controlling tty ") + ctty + "is not a real VT, giving up");
                }
            }
            else
            {
                logger->Err("no controlling tty, giving up");
            }
        }

        if (targetVT <= 0)
        {
            logger->Err("unable to select a VT");
            close(_vtfd);
            return false;
        }

        // Activate the target VT
        if (ioctl(_vtfd, VT_ACTIVATE, targetVT) < 0)
        {
            logger->Err("ioctl VT_ACTIVATE");
            // continue; sometimes activation fails but the vt is usable
        }

        // Wait until the vt becomes active
        if (ioctl(_vtfd, VT_WAITACTIVE, targetVT) < 0)
        {
            logger->Err("ioctl VT_WAITACTIVE (non-fatal)");
            // continue anyway
        }

        // Open the target virtual terminal device (e.g. /dev/tty1)
        char ttyname[32];
        snprintf(ttyname, sizeof(ttyname), "/dev/tty%d", targetVT);
        _ttyfd = open(ttyname, O_RDWR | O_NOCTTY);
        if (_ttyfd < 0)
        {
            logger->Err(std::string("Failed to open target virtual terminal device: ") + ttyname);
            close(_vtfd);
            return false;
        }

        return true;
    }

    // TODO: WE MIGHT NOT NEED THIS
    bool BsodService::RegisterSignals()
    {
        // install simple signal handlers to restore on exit
        /*struct sigaction sa;
        sa.sa_handler = OnExit;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGQUIT, &sa, nullptr);*/
        return true;
    }

    bool BsodService::Paint(const std::string& message) const
    {
        if (_ttyfd < 0)
        {
            // invalid fd
            return false;
        }

        // Get terminal size
        struct winsize ws{};
        if (ioctl(_ttyfd, TIOCGWINSZ, &ws) < 0)
        {
            _serviceManager->Get<Logger::LoggerService>()->Warn("ioctl TIOCGWINSZ " + std::to_string(errno) + " :: using fallback window size");
            ws.ws_row = 25;
            ws.ws_col = 80;
        }

        // Validate winsize values (some drivers may return 0)
        if (ws.ws_row <= 0) ws.ws_row = 25;
        if (ws.ws_col <= 0) ws.ws_col = 80;

        // CSI sequences
        auto hide_cursor = "\033[?25l";
        auto *show_cursor = "\033[?25h";
        auto *clear_home = "\033[2J\033[H";
        auto *set_colors = "\033[44m\033[97m"; // blue background, bright white text
        auto *reset = "\033[0m";

        // Try to hide cursor and clear/set colors -- check errors
        if (!write_all(_ttyfd, hide_cursor)) return false;
        if (!write_all(_ttyfd, clear_home))
        {
            // try to restore cursor before failing
            write_all(_ttyfd, show_cursor);
            return false;
        }
        if (!write_all(_ttyfd, set_colors))
        {
            write_all(_ttyfd, show_cursor);
            return false;
        }

        // Build a full-line string of spaces
        std::string fullLine;
        try
        {
            fullLine.assign(static_cast<size_t>(ws.ws_col), ' ');
        }
        catch (...)
        {
            // allocation failed
            write_all(_ttyfd, reset);
            write_all(_ttyfd, show_cursor);
            return false;
        }

        char posbuf[64];
        for (unsigned r = 0; r < static_cast<unsigned>(ws.ws_row); ++r)
        {
            // Move to 1-based row, column 1
            int needed = snprintf(posbuf, sizeof(posbuf), "\033[%u;1H", r + 1);
            if (needed < 0 || needed >= static_cast<int>(sizeof(posbuf)))
            {
                // truncated or error — restore and fail
                write_all(_ttyfd, reset);
                write_all(_ttyfd, show_cursor);
                return false;
            }
            if (!write_all(_ttyfd, posbuf))
            {
                write_all(_ttyfd, reset);
                write_all(_ttyfd, show_cursor);
                return false;
            }
            if (!write_all(_ttyfd, fullLine))
            {
                write_all(_ttyfd, reset);
                write_all(_ttyfd, show_cursor);
                return false;
            }
            // Carriage return to avoid scrolling issues
            if (!write_all(_ttyfd, "\r"))
            {
                write_all(_ttyfd, reset);
                write_all(_ttyfd, show_cursor);
                return false;
            }
        }

        // Center message
        int msg_row = ws.ws_row / 2;
        int msg_col = (ws.ws_col - static_cast<int>(message.size())) / 2;
        if (msg_col < 0) msg_col = 0;

        int needed = snprintf(posbuf, sizeof(posbuf), "\033[%d;%dH", msg_row + 1, msg_col + 1);
        if (needed < 0 || needed >= static_cast<int>(sizeof(posbuf)))
        {
            write_all(_ttyfd, reset);
            write_all(_ttyfd, show_cursor);
            return false;
        }
        if (!write_all(_ttyfd, posbuf))
        {
            write_all(_ttyfd, reset);
            write_all(_ttyfd, show_cursor);
            return false;
        }

        // Uppercase message safely
        std::string upperMsg = message;
        for (unsigned i = 0; i < upperMsg.size(); ++i)
            upperMsg[i] = static_cast<char>(toupper(static_cast<unsigned char>(upperMsg[i])));

        if (!write_all(_ttyfd, upperMsg))
        {
            write_all(_ttyfd, reset);
            write_all(_ttyfd, show_cursor);
            return false;
        }
        if (!write_all(_ttyfd, "\n\n"))
        {
            write_all(_ttyfd, reset);
            write_all(_ttyfd, show_cursor);
            return false;
        }

        // Instruction line
        const std::string instr = "Press any key to continue...";
        int instr_col = (ws.ws_col - static_cast<int>(instr.size())) / 2;
        if (instr_col < 0) instr_col = 0;
        needed = snprintf(posbuf, sizeof(posbuf), "\033[%d;%dH", msg_row + 3, instr_col + 1);
        if (needed < 0 || needed >= static_cast<int>(sizeof(posbuf)))
        {
            write_all(_ttyfd, reset);
            write_all(_ttyfd, show_cursor);
            return false;
        }
        if (!write_all(_ttyfd, posbuf))
        {
            write_all(_ttyfd, reset);
            write_all(_ttyfd, show_cursor);
            return false;
        }
        if (!write_all(_ttyfd, instr))
        {
            write_all(_ttyfd, reset);
            write_all(_ttyfd, show_cursor);
            return false;
        }

        // Wait until output is transmitted to device
        // tcdrain() is appropriate for TTYs; fsync() is not necessary/meaningful.
        if (tcdrain(_ttyfd) < 0)
        {
            _serviceManager->Get<Logger::LoggerService>()->Warn("tcdrain failed (non-fatal)");
        }

        // Success — leave cursor hidden and colors set.
        return true;
    }

    void BsodService::WaitForSignalOrTimeout() const
    {
        // Wait for a single key press (blocking read) and put tty into raw so we receive single character
        char ch;

        struct termios oldt, newt;
        if (tcgetattr(_ttyfd, &oldt) == 0)
        {
            newt = oldt;
            cfmakeraw(&newt);
            tcsetattr(_ttyfd, TCSANOW, &newt);
            read(_ttyfd, &ch, 1);
            tcsetattr(_ttyfd, TCSANOW, &oldt);
        }
        else
        {
            // if we can't change settings, sleep a bit and exit
            sleep(5);
        }
    }

}
