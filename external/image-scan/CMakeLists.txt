# image-scan - reading the game's own code at runtime.
#
# A static library and not a plugin DLL, for the same reason chat-sdk is: what
# it hands back is std::string and std::vector, and publishing those across two
# independently built DLLs with their own static CRTs is undefined the moment
# one is rebuilt with a different toolset.
#
# It exists because D2R.exe ships with an encrypted .text - uniform byte
# distribution on disk, so nothing static can read it - and a plugin runs inside
# the process where the code has been decrypted. See the header.
add_library(D2RLImageScan STATIC
	src/image_scan.cpp
    src/character_identity.cpp
)

add_library(D2RL::ImageScan ALIAS D2RLImageScan)

target_include_directories(D2RLImageScan PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(D2RLImageScan PUBLIC D2RLPlugin::D2RLPlugin)

set_target_properties(D2RLImageScan PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED TRUE
	CXX_EXTENSIONS FALSE
	ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/image-scan"
	# Must match every plugin that links it - see d2rl_add_plugin.
	MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

if(MSVC)
	target_compile_options(D2RLImageScan PRIVATE
		$<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /EHsc /utf-8>
	)
endif()
