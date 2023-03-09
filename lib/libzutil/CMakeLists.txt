
use_clang()

target_include_directories(libspl BEFORE PUBLIC "")

file(GLOB CSOURCES CONFIGURE_DEPENDS "*.c")
file(GLOB CSOURCES_SUBDIR CONFIGURE_DEPENDS "os/windows/*.c")
set(CSOURCES_COMBINED ${CSOURCES} ${CSOURCES_SUBDIR})

set(TMH_FILE_LIST "")
tmh_generation_for_usermode("${CSOURCES_COMBINED}" TMH_FILE_LIST)

add_library(libzutil
	zutil_device_path.c
	zutil_import.c
	zutil_nicenum.c
	zutil_pool.c
	os/windows/zutil_compat.c
	os/windows/zutil_device_path_os.c
	os/windows/zutil_import_os.c
	${TMH_FILE_LIST}
)

target_link_libraries(libzutil PUBLIC
	libicp
	libnvpair
	libunicode
	libuutil
	libpthread
	libavl
	libefi
	libshare
	libtpool
	zlib
)
