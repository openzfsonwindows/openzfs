
use_clang()

#
# In-process COM server DLL.
# VSS activates software providers (Type=2) via CLSCTX_INPROC_SERVER,
# loading the DLL into vssvc.exe's address space.  The DLL exports
# DllGetClassObject / DllCanUnloadNow; no Windows service is needed.
#
add_library(zfs_vss_provider SHARED
	vss_registry.c
	vss_provider.cpp
)

target_include_directories(zfs_vss_provider PRIVATE
	"${CMAKE_SOURCE_DIR}/cmd/os/windows/zfs_vss_provider"
	"${CMAKE_SOURCE_DIR}/include"
)

target_link_libraries(zfs_vss_provider PRIVATE
	advapi32
	ole32
	oleaut32
	libnvpair
	libspl
	libzpool
	libzfs
)

add_custom_command(
	TARGET zfs_vss_provider
	POST_BUILD
	COMMAND "${SIGNTOOL_PROGRAM}" sign /v /as /fd sha256 /td sha256
		/sha1 "${OPENZFS_SIGNTOOL_SHA1}" /tr "${OPENZFS_SIGNTOOL_TSA}"
		$<TARGET_FILE:zfs_vss_provider>
	WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
	VERBATIM COMMENT "Sign VSS provider DLL"
)

# Installer EXE - registers the DLL as InprocServer32 and VSS provider.
um_add_executable(zfs_vss_install
	vss_install.c
)

target_include_directories(zfs_vss_install PRIVATE
	"${CMAKE_SOURCE_DIR}/cmd/os/windows/zfs_vss_provider"
	"${CMAKE_SOURCE_DIR}/include"
)

target_link_libraries(zfs_vss_install PRIVATE
	advapi32
	ole32
)

install(TARGETS zfs_vss_provider RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES $<TARGET_PDB_FILE:zfs_vss_provider>
	DESTINATION "${CMAKE_INSTALL_BINDIR}"
	OPTIONAL
)
install(TARGETS zfs_vss_install RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
