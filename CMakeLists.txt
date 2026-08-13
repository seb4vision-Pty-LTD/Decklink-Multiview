cmake_minimum_required(VERSION 3.16)
project(DecklinkMultiview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED gstreamer-1.0 gstreamer-video-1.0)

find_package(Qt6 QUIET COMPONENTS Widgets)
if (Qt6_FOUND)
	set(QT_LIBS Qt6::Widgets)
else()
	find_package(Qt5 REQUIRED COMPONENTS Widgets)
	set(QT_LIBS Qt5::Widgets)
endif()

add_executable(DecklinkMultiview DecklinkMultiview.cpp)

target_include_directories(DecklinkMultiview PRIVATE ${GSTREAMER_INCLUDE_DIRS})
target_link_directories(DecklinkMultiview PRIVATE ${GSTREAMER_LIBRARY_DIRS})
target_compile_options(DecklinkMultiview PRIVATE ${GSTREAMER_CFLAGS_OTHER})
target_link_libraries(DecklinkMultiview PRIVATE ${QT_LIBS} ${GSTREAMER_LIBRARIES})
