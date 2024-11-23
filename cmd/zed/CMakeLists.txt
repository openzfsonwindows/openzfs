
use_clang()

um_add_executable(zed
	 zed.c
	 zed_conf.c
	 zed_disk_event.c
	 zed_event.c
	 zed_file.c
	 zed_log.c
	 zed_strings.c
	 agents/fmd_api.c
	 agents/fmd_serd.c
	 agents/zfs_agents.c
	 agents/zfs_diagnosis.c
	 agents/zfs_mod.c
	 agents/zfs_retire.c
	 os/windows/zed_exec.c
	 os/windows/resource.rc
)
target_link_libraries(zed PRIVATE
#	libnvpair
#	libuutil
	libzfs
#	libzfs_core
	libzpool
)

target_include_directories(zed PRIVATE "${CMAKE_SOURCE_DIR}/cmd/zed" "${CMAKE_SOURCE_DIR}/include")

install(TARGETS zed RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES $<TARGET_PDB_FILE:zed>
	DESTINATION "${CMAKE_INSTALL_BINDIR}"
	OPTIONAL
)

# Custom command to generate zed.cat
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cmd/zed/os/windows/zed.cat"
    DEPENDS 
        "${CMAKE_SOURCE_DIR}/cmd/zed/os/windows/zed.inf" 
        "${CMAKE_CURRENT_BINARY_DIR}/zed.exe"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_BINARY_DIR}/zed.exe" "${CMAKE_CURRENT_BINARY_DIR}/cmd/zed/os/windows/zed.exe"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_SOURCE_DIR}/cmd/zed/os/windows/zed.inf" "${CMAKE_CURRENT_BINARY_DIR}/cmd/zed/os/windows/zed.inf.temp"
    COMMAND "${STAMPINF_PROGRAM}" -d "*" -a "amd64" -v "*" -f "${CMAKE_CURRENT_BINARY_DIR}\\cmd\\zed\\os\\windows\\zed.inf.temp"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_BINARY_DIR}/cmd/zed/os/windows/zed.inf.temp" "${CMAKE_CURRENT_BINARY_DIR}/cmd/zed/os/windows/zed.inf"
    COMMAND "${CMAKE_COMMAND}" -E remove "${CMAKE_CURRENT_BINARY_DIR}/cmd/zed/os/windows/zed.inf.temp"
    COMMAND "${INF2CAT_PROGRAM}" /verbose "/driver:${CMAKE_CURRENT_BINARY_DIR}/cmd/zed/os/windows" /os:10_19H1_X86,10_19H1_X64,ServerRS5_X64 /uselocaltime
    COMMAND "${SIGNTOOL_PROGRAM}" sign /v /as /fd sha256 /td sha256 /sha1 "${OPENZFS_SIGNTOOL_SHA1}" /tr "${OPENZFS_SIGNTOOL_TSA}" "${CMAKE_CURRENT_BINARY_DIR}/cmd/zed/os/windows/zed.cat"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cmd/zed/os/windows"
    COMMENT "Generating and signing zed.cat file"
    VERBATIM
)

# Custom target depending on the generated file
add_custom_target(
    zed_cat
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/cmd/zed/os/windows/zed.cat"
    COMMENT "Ensure zed.cat is up to date"
)

add_dependencies(zed_cat zed)
	
install(TARGETS OpenZFS RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}/driver")
install(FILES 
    "${CMAKE_SOURCE_DIR}/cmd/zed/os/windows/zed.inf"
    "${CMAKE_SOURCE_DIR}/cmd/zed/os/windows/zed.cat"
    "${CMAKE_CURRENT_BINARY_DIR}/driver/zed.exe"
	DESTINATION "${CMAKE_INSTALL_BINDIR}/driver"
)
