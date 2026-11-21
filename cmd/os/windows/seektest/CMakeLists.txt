use_clang()

# Test tool for lseek(SEEK_DATA/SEEK_HOLE); not installed.
um_add_executable(seektest seektest.c)

target_link_libraries(seektest PRIVATE libspl)
