
use_clang()

add_definitions(-DWITH_SSL -DINET6)

add_library(libfetch
	common.c
	fetch.c
#	ftp.c
	file.c
	http.c
)

find_library(SSL_EAY_RELEASE
    NAMES
      libssl64MTd
    NAMES_PER_DIR
	HINTS
	  "C:/Program Files/OpenSSL-Win64/lib/VC/static"
    PATH_SUFFIXES
      lib
	REQUIRED
  )

target_link_libraries(libfetch PRIVATE Crypt32.lib ${SSL_EAY_RELEASE})
