cmake_minimum_required(VERSION 3.31)
project(jappeos_notification_manager)

set(CMAKE_CXX_STANDARD 20)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)

include(FetchContent)
FetchContent_Declare(
        jappeos_daemon_core
        GIT_REPOSITORY https://github.com/JappeOS/jappeos_daemon_core.git
        GIT_TAG main-v1.2.2-1
)
FetchContent_MakeAvailable(jappeos_daemon_core)

set(JAPPEOS_NOTIFICATION_MANAGER_SOURCES
        src/main.cpp
        src/notification_manager_service.cpp
        src/notification_manager_service.h
        src/notification_server.cpp
        src/notification_server.h
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

add_executable(jappeos_notification_manager
        ${JAPPEOS_NOTIFICATION_MANAGER_SOURCES}
)
jappeos_configure_target(jappeos_notification_manager)

install(TARGETS jappeos_notification_manager
        RUNTIME DESTINATION "${CMAKE_INSTALL_LIBDIR}/jappeos_notification_manager"
)

install(DIRECTORY data/dbus-1/interfaces/
        DESTINATION "${CMAKE_INSTALL_DATADIR}/dbus-1/interfaces"
        FILES_MATCHING PATTERN "*.xml"
)

install(DIRECTORY data/dbus-1/services/
        DESTINATION "${CMAKE_INSTALL_DATADIR}/dbus-1/services"
        FILES_MATCHING PATTERN "*.service"
)

#install(DIRECTORY data/dbus-1/system.d/
#        DESTINATION "${CMAKE_INSTALL_DATADIR}/dbus-1/system.d"
#        FILES_MATCHING PATTERN "*.conf"
#)

install(DIRECTORY data/systemd/user/
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/systemd/user"
        FILES_MATCHING PATTERN "*.service"
)