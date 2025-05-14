
use_clang()

file(GLOB CSOURCES CONFIGURE_DEPENDS "*.c")

list(REMOVE_ITEM CSOURCES "${CMAKE_CURRENT_SOURCE_DIR}/spl-wmsum.c")

set(TMH_FILE_LIST "")
tmh_generation("${CSOURCES}" TMH_FILE_LIST)
message(STATUS "SPLKERN:- ${TMH_FILE_LIST}")

wdk_add_library(splkern
  spl-atomic.c
  spl-condvar.c
  spl-cred.c
  spl-ddi.c
  spl-debug.c
  spl-err.c
  spl-kmem.c
  spl-kstat.c
  spl-list.c
  spl-md5.c
  spl-mount.c
  spl-mutex.c
  spl-policy.c
  spl-proc.c
  spl-processor.c
  spl-proc_list.c
  spl-rwlock.c
  spl-seg_kmem.c
  spl-taskq.c
  spl-thread.c
  spl-time.c
  spl-tsd.c
  spl-uio.c
  spl-vmem.c
  spl-vnode.c
  spl-windows.c
  spl-xdr.c
  ${TMH_FILE_LIST}
)

target_include_directories(splkern BEFORE PUBLIC "${CMAKE_SOURCE_DIR}/include/os/windows/" PUBLIC "${CMAKE_SOURCE_DIR}/include/os/windows/spl")
# set(CMAKE_TOOLCHAIN_FILE $ENV{CMAKE_TOOLCHAIN_FILE})

find_program(MSVC_CL_EXECUTABLE cl.exe)
if(NOT MSVC_CL_EXECUTABLE)
    message(FATAL_ERROR "MSVC_CL_EXECUTABLE is not set! Make sure to run CMake with a configured compiler.")
else()
    message(STATUS "MSVC_CL_EXECUTABLE is set to: ${MSVC_CL_EXECUTABLE}")
endif()

find_program(MSVC_LIB_EXECUTABLE lib.exe)
if(NOT MSVC_LIB_EXECUTABLE)
    message(FATAL_ERROR "MSVC lib.exe not found in PATH. Ensure you're using Visual Studio Command Prompt or MSVC toolchain.")
else()
message(STATUS "MSVC_LIB_EXECUTABLE is set to: ${MSVC_LIB_EXECUTABLE}")
endif()

set(CR_WRAPPERS_DIR "${CMAKE_BINARY_DIR}/module/os/windows/spl")
file(MAKE_DIRECTORY "${CR_WRAPPERS_DIR}")

set(CR_WRAPPERS_OBJ "${CR_WRAPPERS_DIR}/spl_cr_wrappers.obj")
set(CR_WRAPPERS_LIB "${CR_WRAPPERS_DIR}/spl_cr_wrappers.lib")

add_custom_command(
    OUTPUT ${CR_WRAPPERS_LIB}
    COMMAND ${MSVC_CL_EXECUTABLE} /nologo /c /Fo"${CR_WRAPPERS_OBJ}" ${CMAKE_SOURCE_DIR}/module/os/windows/spl/spl_cr_wrappers.c
    COMMAND ${MSVC_LIB_EXECUTABLE} /nologo /OUT:${CR_WRAPPERS_LIB} /MACHINE:x64 ${CR_WRAPPERS_OBJ}
    WORKING_DIRECTORY ${CR_WRAPPERS_DIR}
    DEPENDS ${CMAKE_SOURCE_DIR}/module/os/windows/spl/spl_cr_wrappers.c
    COMMENT "Building ${CR_WRAPPERS_LIB} in ${CR_WRAPPERS_DIR} using MSVC"
)

add_custom_target(cr_wrappers ALL DEPENDS ${CR_WRAPPERS_LIB})
add_library(spl_cr_wrappers STATIC IMPORTED GLOBAL)
set_target_properties(spl_cr_wrappers PROPERTIES
    IMPORTED_LOCATION ${CR_WRAPPERS_LIB})

