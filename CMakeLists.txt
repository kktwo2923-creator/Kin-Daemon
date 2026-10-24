cmake_minimum_required(VERSION 3.18.1)
project(Kin VERSION 3.2 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

# Android STL selection (static to avoid dependency on system libc++)
if(ANDROID)
    set(ANDROID_STL c++_static)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# ---- 构建开关:免授权调试(仅内部用) ----
# 授权已改为 App 驱动:守护无条件运行, 只读 App 下发的 auth.json 知道是否付费, 不再自行验签。
# -DNO_LICENSE=ON  → 定义 KIN_NO_LICENSE,lic::readAuth() 恒返回已付费(免授权调试)。
# 默认(OFF)       → 读 auth.json 的付费状态(未付费也照常运行, 付费墙在 App 端功能级)。
option(NO_LICENSE "Build with license signal ignored (always paid): for internal debugging" OFF)
if(NO_LICENSE)
    add_compile_definitions(KIN_NO_LICENSE)
    message(STATUS "Kin edition: 公益版 (NO_LICENSE — 授权校验已关闭)")
else()
    message(STATUS "Kin edition: 捐赠版 (licensed — Ed25519 校验启用)")
endif()


# Warnings
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wshadow")

# Release build optimizations (learned from CoreTaskOptimizer)
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT CMAKE_BUILD_TYPE)
    # 注：本进程不做浮点运算，去掉 -ffast-math（它会改变 NaN/Inf 语义、可能引入 UB，对纯整数+sysfs IO 的守护进程毫无收益）
    # [优化 §6.8] IO 密集型常驻 daemon 瓶颈在 sysfs 读写而非算力。-O3+循环展开徒增二进制体积与常驻 RSS。
    #             改 -Os(优化体积):更小的代码段、更低的常驻内存、更快的载入；速度基本无差。保留 -flto。
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -Os -flto")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -finline-functions -fomit-frame-pointer")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fno-rtti -fno-threadsafe-statics -fvisibility=hidden")
    # Remove unused sections to reduce binary size
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -ffunction-sections -fdata-sections")
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} -Wl,--gc-sections -Wl,--strip-all -flto")
endif()

# Debug build flags
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0 -DDEBUG_DURATION=1")
endif()

# The daemon uses Linux/Android system APIs (eventfd, poll, sysfs, and procfs).
# Keep host-only regression tests buildable on Windows without compiling the daemon.
if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Include directories
    include_directories(${CMAKE_SOURCE_DIR}/include)

    # Source files
    # tweetnacl.c 是公有领域 Ed25519 实现(授权验证用),作为独立 C 编译单元,保持原样不混入 C++。
    set(SOURCES
        src/main.cpp
        src/tweetnacl.c
    )

    # Threads (portable)
    find_package(Threads REQUIRED)

    # Executable
    add_executable(Kin ${SOURCES})

    # Link libraries
    target_link_libraries(Kin PRIVATE Threads::Threads)

    # Android-specific link libraries
    if(ANDROID)
        target_link_libraries(Kin PRIVATE log)
    endif()

    # Post-build: strip binary for Android Release builds (learned from CoreTaskOptimizer)
    if(ANDROID AND CMAKE_BUILD_TYPE STREQUAL "Release")
        add_custom_command(TARGET Kin POST_BUILD
            COMMAND ${CMAKE_STRIP} $<TARGET_FILE:Kin>
            COMMENT "Stripping binary for Android release"
        )
    endif()

    # Output directory
    set_target_properties(Kin PROPERTIES
        OUTPUT_NAME "Kinetic"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

# Host regression tests (kept separate from the Android daemon target)
include(CTest)
if(BUILD_TESTING AND NOT ANDROID)
    find_package(Threads REQUIRED)
    add_executable(kin_test_owned_node tests/test_owned_node.cpp)
    target_include_directories(kin_test_owned_node PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_owned_node COMMAND kin_test_owned_node)

    add_executable(kin_test_owned_settings tests/test_owned_settings.cpp)
    target_include_directories(kin_test_owned_settings PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_owned_settings COMMAND kin_test_owned_settings)


    add_executable(kin_test_node_writer tests/test_node_writer.cpp)
    target_include_directories(kin_test_node_writer PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_node_writer COMMAND kin_test_node_writer)

    add_executable(kin_test_write_status tests/test_write_status.cpp)
    target_include_directories(kin_test_write_status PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(kin_test_write_status PRIVATE Threads::Threads)
    add_test(NAME kin_write_status COMMAND kin_test_write_status)

    add_executable(kin_test_config_validation tests/test_config_validation.cpp)
    target_include_directories(kin_test_config_validation PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_config_validation COMMAND kin_test_config_validation)

    add_executable(kin_test_restore_policy tests/test_restore_policy.cpp)
    target_include_directories(kin_test_restore_policy PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_restore_policy COMMAND kin_test_restore_policy)

    add_executable(kin_test_presence_cache tests/test_presence_cache.cpp)
    target_include_directories(kin_test_presence_cache PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_presence_cache COMMAND kin_test_presence_cache)

    add_executable(kin_test_foreground_cache tests/test_foreground_cache.cpp)
    target_include_directories(kin_test_foreground_cache PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(kin_test_foreground_cache PRIVATE Threads::Threads)
    add_test(NAME kin_foreground_cache COMMAND kin_test_foreground_cache)
    set_tests_properties(kin_foreground_cache PROPERTIES TIMEOUT 10)

    add_executable(kin_test_gamelist tests/test_gamelist.cpp)
    target_include_directories(kin_test_gamelist PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_gamelist COMMAND kin_test_gamelist)

    add_executable(kin_test_node_value tests/test_node_value.cpp)
    target_include_directories(kin_test_node_value PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_node_value COMMAND kin_test_node_value)

    add_executable(kin_test_thermal_status tests/test_thermal_status.cpp)
    target_include_directories(kin_test_thermal_status PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_thermal_status COMMAND kin_test_thermal_status)

    add_executable(kin_test_battery_policy tests/test_battery_policy.cpp)
    target_include_directories(kin_test_battery_policy PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_battery_policy COMMAND kin_test_battery_policy)

    add_executable(kin_test_policy_status tests/test_policy_status.cpp)
    target_include_directories(kin_test_policy_status PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_policy_status COMMAND kin_test_policy_status)

    add_executable(kin_test_policy_resolver tests/test_policy_resolver.cpp)
    target_include_directories(kin_test_policy_resolver PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_policy_resolver COMMAND kin_test_policy_resolver)

    add_executable(kin_test_autofsm tests/test_autofsm.cpp)
    target_include_directories(kin_test_autofsm PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/tests)
    target_link_libraries(kin_test_autofsm PRIVATE Threads::Threads)
    add_test(NAME kin_autofsm COMMAND kin_test_autofsm)

    add_executable(kin_test_smart_learning tests/test_smart_learning.cpp)
    target_include_directories(kin_test_smart_learning PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_smart_learning COMMAND kin_test_smart_learning)

    add_executable(kin_test_probe_cadence tests/test_probe_cadence.cpp)
    target_include_directories(kin_test_probe_cadence PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_probe_cadence COMMAND kin_test_probe_cadence)

    add_executable(kin_test_load_governor tests/test_load_governor.cpp)
    target_include_directories(kin_test_load_governor PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_load_governor COMMAND kin_test_load_governor)

    add_executable(kin_test_fas_frames tests/test_fas_frames.cpp)
    target_include_directories(kin_test_fas_frames PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_fas_frames COMMAND kin_test_fas_frames)

    add_executable(kin_test_idle_uclamp tests/test_idle_uclamp.cpp)
    target_include_directories(kin_test_idle_uclamp PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_idle_uclamp COMMAND kin_test_idle_uclamp)

    add_executable(kin_test_thermal_decision tests/test_thermal_decision.cpp)
    target_include_directories(kin_test_thermal_decision PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_thermal_decision COMMAND kin_test_thermal_decision)

    add_executable(kin_test_power_boost_node tests/test_power_boost_node.cpp)
    target_include_directories(kin_test_power_boost_node PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_power_boost_node COMMAND kin_test_power_boost_node)

    add_executable(kin_test_window_focus tests/test_window_focus.cpp)
    target_include_directories(kin_test_window_focus PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_window_focus COMMAND kin_test_window_focus)
    add_executable(kin_test_appprofile tests/test_appprofile.cpp)
    target_include_directories(kin_test_appprofile PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_appprofile COMMAND kin_test_appprofile)

    add_executable(kin_test_skin_thermal tests/test_skin_thermal.cpp)
    target_include_directories(kin_test_skin_thermal PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_skin_thermal COMMAND kin_test_skin_thermal)

    add_executable(kin_test_package_name tests/test_package_name.cpp)
    target_include_directories(kin_test_package_name PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_package_name COMMAND kin_test_package_name)

    add_executable(kin_test_wifi_power_save tests/test_wifi_power_save.cpp)
    target_include_directories(kin_test_wifi_power_save PRIVATE ${CMAKE_SOURCE_DIR}/include)
    add_test(NAME kin_wifi_power_save COMMAND kin_test_wifi_power_save)

    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME kin_build_pack
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_build_pack.py)
        add_test(NAME kin_module_scripts
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/test_review_scripts.py)
        add_executable(kin_test_metrics_clients tests/test_metrics_clients.cpp)
        target_include_directories(kin_test_metrics_clients PRIVATE ${CMAKE_SOURCE_DIR}/include)
        target_link_libraries(kin_test_metrics_clients PRIVATE Threads::Threads)
        add_test(NAME kin_metrics_clients COMMAND kin_test_metrics_clients)
        set_tests_properties(kin_metrics_clients PROPERTIES TIMEOUT 20)

        add_executable(kin_test_syswrite_existing tests/test_syswrite_existing.cpp)
        target_include_directories(kin_test_syswrite_existing PRIVATE ${CMAKE_SOURCE_DIR}/include)
        add_test(NAME kin_syswrite_existing COMMAND kin_test_syswrite_existing)
    endif()
endif()

# Installation
if(TARGET Kin)
    install(TARGETS Kin DESTINATION bin)
endif()
