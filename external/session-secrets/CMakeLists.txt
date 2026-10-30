# session-secrets - bearer tokens and server keys read from
# <game folder>/reimagined-secrets/ instead of the mod folder, which D2RLoader
# hands to every player who joins a host. See include/D2RLSecrets/session_secrets.h.
#
# A static library for the same reason level-names is: it hands back
# std::string, which must not cross two independently built DLLs.
add_library(D2RLSessionSecrets STATIC
	src/session_secrets.cpp
)

add_library(D2RL::SessionSecrets ALIAS D2RLSessionSecrets)

target_include_directories(D2RLSessionSecrets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(D2RLSessionSecrets PUBLIC D2RLPlugin::D2RLPlugin)

set_target_properties(D2RLSessionSecrets PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED TRUE
	CXX_EXTENSIONS FALSE
	ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/session-secrets"
	# Must match every plugin that links it - see d2rl_add_plugin.
	MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

if(MSVC)
	target_compile_options(D2RLSessionSecrets PRIVATE
		$<$<COMPILE_LANGUAGE:CXX>:/W4 /permissive- /EHsc /utf-8>
	)
endif()
