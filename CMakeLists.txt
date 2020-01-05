cmake_minimum_required(VERSION 3.3)
project(robotraconteur_quictransport)

IF (MSVC)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4251 /wd4275 /bigobj")
ENDIF()

include_directories(${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/include)

SET (Boost_USE_MULTITHREADED ON)
SET (Boost_USE_STATIC_RUNTIME OFF)
FIND_PACKAGE(Boost COMPONENTS date_time filesystem system regex chrono atomic thread random program_options REQUIRED)
include_directories(${Boost_INCLUDE_DIRS})
add_definitions(-DBOOST_ALL_NO_LIB)

find_package(picoquic REQUIRED)

add_definitions(-D_WIN32_WINNT=0x0601)

find_package(RobotRaconteur REQUIRED)

find_path(RobotRaconteur_src_dir
    NAMES RobotRaconteurCore/src/ASIOStreamBaseTransport.cpp
)

include_directories( ${RobotRaconteur_INCLUDE_DIR} 
    ${RobotRaconteur_src_dir}/RobotRaconteurCore/include 
    ${RobotRaconteur_src_dir}/RobotRaconteurCore/src    
    ${RobotRaconteur_src_dir}/testing/RobotRaconteurTest
)

add_library(RobotRaconteurCore_QuicTransport STATIC 
    src/QuicTransport.cpp
    src/QuicTransport_private.h
    include/RobotRaconteur/QuicTransport.h    
)
target_link_libraries(RobotRaconteurCore_QuicTransport 
    RobotRaconteurCore 
    picoquic-core
    ${Boost_LIBRARIES}  ${ROBOTRACONTEUR_EXTRA_LIBS}
)


ROBOTRACONTEUR_GENERATE_THUNK(RR_THUNK_SRCS RR_THUNK_HDRS 
${RobotRaconteur_src_dir}/testing/RobotRaconteurTest/com.robotraconteur.testing.TestService1.robdef 
${RobotRaconteur_src_dir}/testing/RobotRaconteurTest/com.robotraconteur.testing.TestService2.robdef 
${RobotRaconteur_src_dir}/testing/RobotRaconteurTest/com.robotraconteur.testing.TestService3.robdef
    
	MASTER_HEADER
)

add_executable(RobotRaconteurQuicTest 
    ${RobotRaconteur_src_dir}/testing/RobotRaconteurTest/CompareArray.cpp
    ${RobotRaconteur_src_dir}/testing/RobotRaconteurTest/ServiceTest.cpp
    ${RobotRaconteur_src_dir}/testing/RobotRaconteurTest/ServiceTest2.cpp
    ${RobotRaconteur_src_dir}/testing/RobotRaconteurTest/ServiceTestClient.cpp
    ${RobotRaconteur_src_dir}/testing/RobotRaconteurTest/ServiceTestClient2.cpp
    ${RobotRaconteur_src_dir}/testing/RobotRaconteurTest/MultiDimArrayTest.cpp
    test/RobotRaconteurQuicTest.cpp
    ${RR_THUNK_SRCS} ${RR_THUNK_HDRS}
)

target_link_libraries(RobotRaconteurQuicTest
    RobotRaconteurCore_QuicTransport RobotRaconteurCore
    picoquic-core
    ${Boost_LIBRARIES}  ${ROBOTRACONTEUR_EXTRA_LIBS}
)
