cmake_minimum_required(VERSION 3.31)
project(jappeos_core)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -rdynamic -g -O0 -march=x86-64") # TODO

find_package(PkgConfig REQUIRED)

# Use pkg-config to find dbus-1
pkg_check_modules(DBUS REQUIRED dbus-1)

# Find PAM
pkg_check_modules(PAM REQUIRED pam)

# Add include directories
include_directories(
        ${DBUS_INCLUDE_DIRS}
        ${DBUS_INCLUDE_DIRS}/../dbus-1.0    # Handles /usr/include/dbus-1.0
        ${DBUS_LIBDIR}/dbus-1.0/include     # Handles /usr/lib/dbus-1.0/include
        ${PAM_INCLUDE_DIRS}
)

# Add source files
add_executable(jappeos_core src/main.cpp
        src/application.cpp
        src/application.h
        src/services/service.h
        src/services/logger/logger_service.h
        src/services/logger/stdout_logger.cpp
        src/services/logger/stdout_logger.h
        src/globals.h
        src/application_state.def
        src/services/session_manager/session_manager_service.h
        src/services/session_manager/session_manager_service.cpp
        src/services/service.cpp
        src/services/watchdog/watchdog_service.cpp
        src/services/watchdog/watchdog_service.h
        src/event.h
        src/services/bsod/bsod_service.cpp
        src/services/bsod/bsod_service.h
        src/utils/jos_exception.h
        src/services/installer/installer_service.cpp
        src/services/installer/installer_service.h
        src/services/power_manager/power_manager_service.cpp
        src/services/power_manager/power_manager_service.h)

# Link dbus and PAM
target_link_libraries(jappeos_core ${DBUS_LIBRARIES})
target_link_libraries(jappeos_core ${PAM_LIBRARIES})

# Silence pkg-config warnings
add_definitions(${DBUS_CFLAGS_OTHER})
add_definitions(${PAM_CFLAGS_OTHER})