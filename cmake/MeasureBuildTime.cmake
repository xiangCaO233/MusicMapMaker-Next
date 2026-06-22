cmake_minimum_required(VERSION 3.31)

get_filename_component(_MMM_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT DEFINED MMM_BUILD_DIR OR MMM_BUILD_DIR STREQUAL "")
	set(MMM_BUILD_DIR "${_MMM_REPO_ROOT}/build")
endif()

if(NOT IS_ABSOLUTE "${MMM_BUILD_DIR}")
	get_filename_component(MMM_BUILD_DIR "${MMM_BUILD_DIR}" ABSOLUTE BASE_DIR "${_MMM_REPO_ROOT}")
endif()

file(TO_CMAKE_PATH "${MMM_BUILD_DIR}" MMM_BUILD_DIR)

if(NOT EXISTS "${MMM_BUILD_DIR}/CMakeCache.txt")
	message(FATAL_ERROR "MMM_BUILD_DIR does not contain CMakeCache.txt: ${MMM_BUILD_DIR}")
endif()

if(NOT DEFINED MMM_CLEAN)
	set(MMM_CLEAN OFF)
endif()

if(NOT DEFINED MMM_PARALLEL)
	set(MMM_PARALLEL "")
endif()

if(NOT DEFINED MMM_TARGET)
	set(MMM_TARGET "")
endif()

if(MMM_CLEAN)
	message(STATUS "Cleaning build directory: ${MMM_BUILD_DIR}")
	execute_process(
		COMMAND "${CMAKE_COMMAND}" --build "${MMM_BUILD_DIR}" --target clean
		RESULT_VARIABLE _MMM_CLEAN_RESULT
	)

	if(NOT _MMM_CLEAN_RESULT EQUAL 0)
		message(FATAL_ERROR "Clean failed with exit code ${_MMM_CLEAN_RESULT}.")
	endif()
endif()

set(_MMM_BUILD_COMMAND "${CMAKE_COMMAND}" "--build" "${MMM_BUILD_DIR}")

if(NOT MMM_TARGET STREQUAL "")
	list(APPEND _MMM_BUILD_COMMAND "--target" "${MMM_TARGET}")
endif()

if(NOT MMM_PARALLEL STREQUAL "")
	list(APPEND _MMM_BUILD_COMMAND "--parallel" "${MMM_PARALLEL}")
endif()

string(REPLACE ";" " " _MMM_BUILD_COMMAND_TEXT "${_MMM_BUILD_COMMAND}")
message(STATUS "Timed build command: ${_MMM_BUILD_COMMAND_TEXT}")
message(STATUS "Timer command: ${CMAKE_COMMAND} -E time")

execute_process(
	COMMAND "${CMAKE_COMMAND}" -E time ${_MMM_BUILD_COMMAND}
	RESULT_VARIABLE _MMM_BUILD_RESULT
)

if(NOT _MMM_BUILD_RESULT EQUAL 0)
	message(FATAL_ERROR "Timed build failed with exit code ${_MMM_BUILD_RESULT}.")
endif()
