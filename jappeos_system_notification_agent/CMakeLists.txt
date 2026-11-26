cmake_minimum_required(VERSION 3.31)
project(jappeos_system_notification_agent)

set(CMAKE_CXX_STANDARD 20)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)

include(FetchContent)
FetchContent_Declare(
        jappeos_daemon_core
        GIT_REPOSITORY https://github.com/JappeOS/jappeos_daemon_core.git
        GIT_TAG main-v1.5.0-1
)
FetchContent_MakeAvailable(jappeos_daemon_core)

set(JAPPEOS_SYSTEM_NOTIFICATION_AGENT_SOURCES
        src/main.cpp
        src/notification_agent_service.cpp
        src/notification_agent_service.h
        src/notification_agent.cpp
        src/notification_agent.h
        src/service_watcher.cpp
        src/service_watcher.h
        src/agents/driver_manager_agent.cpp
        src/agents/driver_manager_agent.h
        src/agents/power_manager_agent.cpp
        src/agents/power_manager_agent.h
)

function(jappeos_configure_target target_name)
    #target_include_directories(${target_name} PRIVATE
    #)

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
    )

    target_link_options(${target_name} PRIVATE
            $<$<CONFIG:Debug>:-rdynamic>
    )

    target_link_libraries(${target_name} PRIVATE
            jappeos_daemon_core::jappeos_daemon_core
    )

    set_target_properties(${target_name} PROPERTIES
            INSTALL_RPATH "$ORIGIN"
            BUILD_WITH_INSTALL_RPATH TRUE
    )
endfunction()

add_executable(jappeos_system_notification_agent
        ${JAPPEOS_SYSTEM_NOTIFICATION_AGENT_SOURCES}
)
jappeos_configure_target(jappeos_system_notification_agent)

install(TARGETS jappeos_system_notification_agent
        RUNTIME DESTINATION "${CMAKE_INSTALL_LIBDIR}/jappeos_system_notification_agent"
)

install(DIRECTORY data/systemd/user/
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/user"
        FILES_MATCHING PATTERN "*.service"
)
