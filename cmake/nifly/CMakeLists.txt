# Builds nifly from the submodule sources directly; its own CMake adds install/export rules
# and project-level variables FasterNGIO does not need.
set(NIFLY_ROOT "${PROJECT_SOURCE_DIR}/external/nifly")
file(GLOB NIFLY_SOURCES CONFIGURE_DEPENDS "${NIFLY_ROOT}/src/*.cpp")
add_library(nifly STATIC ${NIFLY_SOURCES})
target_include_directories(nifly PUBLIC "${NIFLY_ROOT}/include")
target_include_directories(nifly SYSTEM PUBLIC "${NIFLY_ROOT}/external")
target_compile_features(nifly PUBLIC cxx_std_17)
if(MSVC)
	target_compile_options(nifly PRIVATE /utf-8 /W0 /Zc:inline)
	target_compile_options(nifly PUBLIC /EHsc /bigobj)
endif()
add_library(nifly::nifly ALIAS nifly)
