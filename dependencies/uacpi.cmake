# dependencies/uacpi.cmake

# Copyright (c) 2024 Omar Berrow

if (OBOS_REFRESH_DEPENDENCIES)
	include(FetchContent)

	FetchContent_Declare(uACPI
		GIT_REPOSITORY https://github.com/UltraOS/uACPI.git
		GIT_TAG 4d4d5d691756a6c9ff8ddfbd0c579786483a5acb
		SOURCE_DIR ${CMAKE_SOURCE_DIR}/dependencies/uACPI
	)
	FetchContent_MakeAvailable(uACPI)
	
	file (COPY "${CMAKE_SOURCE_DIR}/dependencies/uACPI/include/uacpi" DESTINATION "${CMAKE_SOURCE_DIR}/dependencies/include/")
endif()
set (uacpi_cmake_file ${CMAKE_SOURCE_DIR}/dependencies/uACPI/uacpi.cmake CACHE INTERNAL "The uACPI CMake file.")
