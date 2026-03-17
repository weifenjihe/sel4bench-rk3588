cmake_minimum_required(VERSION 3.16.0)

project(virtio_demo C)

set(configure_string "")

config_option(
    AppVirtioDemo
    APP_VIRTIODEMO
    "Application to demonstrate VirtIO communication with Linux."
    DEFAULT ON
)

# 像 sel4bench 一样引入所需的包
find_package(musllibc REQUIRED)
find_package(util_libs REQUIRED)
find_package(seL4_libs REQUIRED)
find_package(sel4_projects_libs REQUIRED)
find_package(projects_libs REQUIRED)

# 设置构建环境和导入库，这部分复用了 sel4bench 的逻辑
musllibc_setup_build_environment_with_sel4runtime()
sel4_import_libsel4()
util_libs_import_libraries()

# 设置一些必要的缓存变量
set(LibSel4MuslcSysMorecoreBytes
    0
    CACHE STRING "" FORCE
)

# Force Kernel Debug Build ON so seL4_DebugPutChar is available
set(KernelPrinting
    ON
    CACHE BOOL "" FORCE
)
set(KernelDebugBuild
    ON
    CACHE BOOL "" FORCE
)

sel4_libs_import_libraries()
projects_libs_import_libraries()

set(LibNanopb
    ON
    CACHE BOOL "" FORCE
)
sel4_projects_libs_import_libraries()

# 引入 sel4benchsupport 库，以便将来进行测试
add_subdirectory(../../libsel4benchsupport libsel4benchsupport)

# Force Kernel Printing ON for debugging
set(KernelPrinting
    ON
    CACHE BOOL "" FORCE
)
# Force Kernel Debug Build ON so seL4_DebugPutChar is available
set(KernelDebugBuild
    ON
    CACHE BOOL "" FORCE
)
set(KernelVerificationBuild
    OFF
    CACHE BOOL "" FORCE
)

message(STATUS "Configuring virtio_demo app...")
add_config_library(virtio_demo "${configure_string}")

# 只需要 main.c
add_executable(virtio_demo src/main.c src/virtio_driver.c src/virtio_perf.c)

# 链接必要的库，包括 sel4benchsupport 和 virtio
target_link_libraries(
    virtio_demo
    sel4benchsupport
    sel4
    sel4muslcsys
    sel4utils
    sel4platsupport
    sel4simple
    sel4simple-default
    sel4allocman
    sel4vka
    sel4vspace
    sel4_autoconf
    virtio
)

include(rootserver)
declarerootserver(virtio_demo)

