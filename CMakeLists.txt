PROJECT(corridor_navigation)
cmake_minimum_required(VERSION 2.6)

execute_process(COMMAND ${CMAKE_CXX_COMPILER} -dumpversion OUTPUT_VARIABLE GCC_VERSION)
if (GCC_VERSION VERSION_EQUAL 4.9.1)
    #the corridor_navigation triggers compiler bug on 4.9.1
	SET(CMAKE_CXX_COMPILER /usr/bin/g++-4.8)
endif()

SET (CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/.orogen/config")
INCLUDE(corridor_navigationBase)

# FIND_PACKAGE(KDL)
# FIND_PACKAGE(OCL)

