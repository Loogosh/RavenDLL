cmake_minimum_required(VERSION 3.16)
project(raven_dll LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "RavenDLL requires x64 (WowClassic 1.14 is amd64).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

# --- SQLite (item database backend) ---
add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)
target_include_directories(sqlite3 PUBLIC third_party/sqlite)

# --- SlotPrep.dll: reserves FrameScript slots, hands them out via ClaimSlot ---
add_library(SlotPrep SHARED plugins/SlotPrep/SlotPrep.cpp)
target_link_libraries(SlotPrep PRIVATE psapi)

# --- SuperWoW.dll: engine layer + public gate ---
add_library(SuperWoW SHARED plugins/SuperWoW/SuperWoW.cpp)

# --- HearthDB.dll: SQLite engine exposed as HDB_* ---
add_library(HearthDB SHARED
	plugins/HearthDB/HearthDB.cpp
	plugins/HearthDB/DbEngine.cpp
)
target_link_libraries(HearthDB PRIVATE sqlite3)
target_compile_definitions(HearthDB PRIVATE UNICODE _UNICODE)
target_link_options(HearthDB PRIVATE "/MAP:${CMAKE_BINARY_DIR}/HearthDB.map")

# --- version.dll: sideload proxy that loads the stack early ---
add_library(VersionProxy SHARED plugins/VersionProxy/VersionProxy.cpp plugins/VersionProxy/version.def)
set_target_properties(VersionProxy PROPERTIES OUTPUT_NAME "version")
target_link_options(VersionProxy PRIVATE "/MAP:${CMAKE_BINARY_DIR}/version.map")

set_target_properties(SlotPrep SuperWoW HearthDB VersionProxy PROPERTIES
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
	LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
	ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
)
foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
	foreach(t SlotPrep SuperWoW HearthDB VersionProxy)
		set_target_properties(${t} PROPERTIES
			RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/out"
			LIBRARY_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/out"
			ARCHIVE_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/out"
		)
	endforeach()
endforeach()
