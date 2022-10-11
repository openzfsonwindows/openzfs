
use_clang()

um_add_executable(winfstest
	winfstest.c
)

target_link_libraries(winfstest PRIVATE
)
install(TARGETS winfstest RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES $<TARGET_PDB_FILE:winfstest>
	DESTINATION "${CMAKE_INSTALL_BINDIR}"
	OPTIONAL
)
