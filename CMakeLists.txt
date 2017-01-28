cmake_minimum_required( VERSION 3.0 )

project(lockfree-set VERSION 0.1)
set(Project_SHORTCUT "lf")
set(Project_PREFIX "lf")
set(Project_MEDIA_DIR "media")

add_subdirectory("extras/buildx")
#buildx_enable_debug(all)
buildx_set_default_prefixes()
buildx_set_default_output_dirs()
buildx_activate_cpp11()
buildx_enable_pch()

if(WIN32)
	SET(Boost_USE_STATIC_LIBS ON)
endif()
find_package(Boost COMPONENTS system thread regex unit_test_framework filesystem REQUIRED)
include_directories(${Boost_INCLUDE_DIRS})
link_directories(${Boost_LIBRARY_DIRS})

find_package(logx REQUIRED)
find_package(utilx REQUIRED)

#include_directories("tests")

# tests must come first
add_subdirectory("src")
