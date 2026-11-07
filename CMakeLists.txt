cmake_minimum_required(VERSION 3.24)
project(pkteditor LANGUAGES CXX)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets)
set(CMAKE_CXX_COMPILER /usr/sbin/clang++)
set(CMAKE_CXX_STANDARD 20)

set(PKT_SRC pmap.cpp transport.cpp)

add_executable(pktest test_main.cpp ${PKT_SRC})
add_executable(pkteditor gui_main.cpp ${PKT_SRC})

foreach(t IN ITEMS pktest pkteditor)
  target_include_directories(${t} PRIVATE /usr/include/qt6 /usr/include/qt6/QtCore)
endforeach()
target_include_directories(pkteditor PRIVATE /usr/include/qt6/QtWidgets)

find_library(USB_LIB NAMES usb-1.0)
target_link_libraries(pktest PRIVATE Qt6Core "${USB_LIB}")
target_link_libraries(pkteditor PRIVATE Qt6Core Qt6Widgets "${USB_LIB}")
