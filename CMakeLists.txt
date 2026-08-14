file(READ "${CMAKE_CURRENT_LIST_DIR}/pack.json" PACK_JSON)
string(JSON PACK_NAME GET "${PACK_JSON}" name)

if(NOT TARGET rigImGui)
	if(DEFINED RIGKIT_ROOT AND NOT RIGKIT_ROOT STREQUAL "")
		set(_rigImGui_dir "${RIGKIT_ROOT}/packs/rigImGui")
	else()
		set(_rigImGui_dir "${CMAKE_SOURCE_DIR}/packs/rigImGui")
	endif()
	if(EXISTS "${_rigImGui_dir}/CMakeLists.txt")
		add_subdirectory("${_rigImGui_dir}" "${CMAKE_BINARY_DIR}/packs/rigImGui")
	else()
		message(FATAL_ERROR "rigNetScan requires rigImGui under packs/rigImGui")
	endif()
endif()

add_library(${PACK_NAME} STATIC
	src/rigNetScan.cpp
	src/rigNetScan.h
	src/NetIo.cpp
	src/NetIo.h
	src/CNetScan.h
	src/CNetHost.h
	src/IpScannerWindow.cpp
	src/IpScannerWindow.h
)
target_link_libraries(${PACK_NAME} PUBLIC rigkit rigImGui)
if(WIN32)
	target_link_libraries(${PACK_NAME} PRIVATE ws2_32 iphlpapi)
endif()
# Handle2D in rigImGui calls PrimitiveBounds in rigComponent; GNU ld needs the
# provider after the requester. Repeating rigComponent here on the exe line.
if(TARGET rigComponent)
	target_link_libraries(${PACK_NAME} PRIVATE rigComponent)
endif()
target_include_directories(${PACK_NAME} PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/src>
)
message(STATUS "Configured pack: ${PACK_NAME} (LAN TCP scan)")
