
use_clang()

# Out-of-process COM server EXE.
# VSS activates software providers (Type=2) via CLSCTX_LOCAL_SERVER,
# so the provider must be a LocalServer32 EXE, not an InprocServer32 DLL.
um_add_executable(zfs_vss_provider
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

# Installer EXE - registers the EXE as LocalServer32 and VSS provider.
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
