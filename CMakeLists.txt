cmake_minimum_required(VERSION 3.31)
project(jappeos_core)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)

pkg_check_modules(DBUS REQUIRED dbus-1)
pkg_check_modules(PAM REQUIRED pam)
pkg_check_modules(LIBNM REQUIRED libnm)

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
        src/services/power_manager/power_manager_service.h
        src/utils/dbus_utils.h
        src/services/account_manager/account_manager_service.cpp
        src/services/account_manager/account_manager_service.h
        src/services/network_manager/network_manager_service.cpp
        src/services/network_manager/network_manager_service.h
        src/dbus/dbus_wrapper.h
        src/dbus/dbus_wrapper.cpp
        src/services/network_manager/network_device.h
        src/services/network_manager/network_access_point.h
        src/services/network_manager/network_device.cpp
        src/services/network_manager/network_access_point.cpp
        src/services/network_manager/network_connection.cpp
        src/services/network_manager/network_connection.h
        src/services/network_manager/network_device_def.h
        src/services/network_manager/network_connection_def.h)

# Add include directories
target_include_directories(jappeos_core PRIVATE
        ${DBUS_INCLUDE_DIRS}
        ${DBUS_INCLUDE_DIRS}/../dbus-1.0
        ${DBUS_LIBDIR}/dbus-1.0/include
        ${PAM_INCLUDE_DIRS}
        ${LIBNM_INCLUDE_DIRS}
)

target_compile_options(jappeos_core PRIVATE
        -march=x86-64
        -mtune=generic
        -fno-lto
        -mno-avx
        -mno-avx2
        -mno-fma
        -mno-bmi
        -mno-bmi2
        -mno-sse4.1
        -mno-sse4.2
        $<$<CONFIG:Debug>:-O0 -g>
        $<$<CONFIG:Release>:-O2 -g -fno-omit-frame-pointer>
        ${DBUS_CFLAGS_OTHER}
        ${PAM_CFLAGS_OTHER}
        ${LIBNM_CFLAGS_OTHER}
)

target_link_options(jappeos_core PRIVATE
        $<$<CONFIG:Debug>:-rdynamic>
)

target_link_libraries(jappeos_core PRIVATE
        ${DBUS_LIBRARIES}
        ${PAM_LIBRARIES}
        ${LIBNM_LIBRARIES}
)
