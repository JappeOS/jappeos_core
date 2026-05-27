cmake_minimum_required(VERSION 3.31)
project(jappeos_core)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)

pkg_check_modules(DBUS REQUIRED dbus-1)
pkg_check_modules(PAM REQUIRED pam)
pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(LIBNM REQUIRED libnm)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(PULSE REQUIRED libpulse)
pkg_check_modules(PULSE_GLIB REQUIRED libpulse-mainloop-glib)

set(JAPPEOS_COMMON_SOURCES
        src/main.cpp
        src/application.cpp
        src/application.h
        src/globals.h
        src/application_state.def
        src/event.h
        src/logger.cpp
        src/logger.h
        src/dbus/dbus_wrapper.h
        src/dbus/dbus_wrapper.cpp
        src/services/service.h
        src/services/service.cpp
        src/services/logger/logger_service.h
        src/services/logger/stdout_logger.cpp
        src/services/logger/stdout_logger.h
        src/services/watchdog/watchdog_service.cpp
        src/services/watchdog/watchdog_service.h
        src/utils/jos_exception.h
        src/utils/dbus_utils.h
)

set(JAPPEOS_SYSTEM_SOURCES
        src/services/account_manager/account_manager_service.cpp
        src/services/account_manager/account_manager_service.h
        src/services/power_manager/power_manager_service.cpp
        src/services/power_manager/power_manager_service.h
        src/services/power_manager/power_device.cpp
        src/services/power_manager/power_device.h
        src/services/network_manager/network_manager_service.cpp
        src/services/network_manager/network_manager_service.h
        src/services/network_manager/network_device.h
        src/services/network_manager/network_access_point.h
        src/services/network_manager/network_device.cpp
        src/services/network_manager/network_access_point.cpp
        src/services/network_manager/network_connection.cpp
        src/services/network_manager/network_connection.h
        src/services/network_manager/network_device_def.h
        src/services/network_manager/network_connection_def.h
        src/services/installer/installer_service.cpp
        src/services/installer/installer_service.h
        src/services/bsod/bsod_service.cpp
        src/services/bsod/bsod_service.h
        src/services/session_manager/session_manager_service.cpp
        src/services/session_manager/session_manager_service.h
        src/utils/scope_guard.h
)

set(JAPPEOS_SESSION_SOURCES
        src/services/session_host/session_host_service.cpp
        src/services/session_host/session_host_service.h
        src/services/audio/audio_service.cpp
        src/services/audio/audio_service.h
        src/services/audio/audio_device.cpp
        src/services/audio/audio_device.h
        src/services/audio/audio_stream.cpp
        src/services/audio/audio_stream.h
        src/services/audio_pulse/audio_service.cpp
        src/services/audio_pulse/audio_service.h
        src/services/audio_pulse/audio_device.cpp
        src/services/audio_pulse/audio_device.h
        src/services/audio_pulse/audio_stream.cpp
        src/services/audio_pulse/audio_stream.h
)

function(jappeos_configure_target target_name)
    target_include_directories(${target_name} PRIVATE
            ${DBUS_INCLUDE_DIRS}
            ${DBUS_INCLUDE_DIRS}/../dbus-1.0
            ${DBUS_LIBDIR}/dbus-1.0/include
            ${PAM_INCLUDE_DIRS}
            ${GLIB_INCLUDE_DIRS}
            ${LIBNM_INCLUDE_DIRS}
            ${PIPEWIRE_INCLUDE_DIRS}
            ${PULSE_INCLUDE_DIRS}
            ${PULSE_GLIB_INCLUDE_DIRS}
    )

    target_compile_options(${target_name} PRIVATE
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
            ${GLIB_CFLAGS_OTHER}
            ${LIBNM_CFLAGS_OTHER}
            ${PIPEWIRE_CFLAGS_OTHER}
            ${PULSE_CFLAGS_OTHER}
            ${PULSE_GLIB_CFLAGS_OTHER}
    )

    target_link_options(${target_name} PRIVATE
            $<$<CONFIG:Debug>:-rdynamic>
    )

    target_link_libraries(${target_name} PRIVATE
            ${DBUS_LIBRARIES}
            ${PAM_LIBRARIES}
            ${GLIB_LIBRARIES}
            ${LIBNM_LIBRARIES}
            ${PIPEWIRE_LIBRARIES}
            ${PULSE_LIBRARIES}
            ${PULSE_GLIB_LIBRARIES}
    )
endfunction()

add_executable(jappeos_core
        ${JAPPEOS_COMMON_SOURCES}
        ${JAPPEOS_SYSTEM_SOURCES}
)
target_compile_definitions(jappeos_core PRIVATE JAPPEOS_DAEMON_SYSTEM)
jappeos_configure_target(jappeos_core)

add_executable(jappeos_session
        ${JAPPEOS_COMMON_SOURCES}
        ${JAPPEOS_SESSION_SOURCES}
)
target_compile_definitions(jappeos_session PRIVATE JAPPEOS_DAEMON_SESSION)
jappeos_configure_target(jappeos_session)
