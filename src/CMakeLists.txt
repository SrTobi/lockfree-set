
################### setup include directories ###################
include_directories(${CMAKE_CURRENT_SOURCE_DIR})



################### scan for source ##################

buildx_scan_here(LOCKFREESET_SOURCE "cpp;hpp")
buildx_debug("source: ${LOCKFREESET_SOURCE}" proj)
buildx_auto_group("${LOCKFREESET_SOURCE}")

add_executable(lockfree-set ${LOCKFREESET_SOURCE})
target_link_libraries(lockfree-set ${Boost_LIBRARIES} x::utilx)

# tests
#AUTO_SOURCE_GROUP("${TEST_SOURCE}")
#add_executable(test-axion ${SOURCE} ${TEST_SOURCE})
#ADD_PRECOMPILED_HEADER(test-axion "includes.hpp")
#target_link_libraries(test-axion ${Boost_LIBRARIES})