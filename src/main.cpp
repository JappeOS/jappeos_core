#include "application.h"

int main()
{
    JappeStudios::JappeOS::JappeOSCore::Application app;
    const uint8_t exitCode = app.Run();
    return exitCode;
}
