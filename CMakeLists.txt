cmake_minimum_required(VERSION 3.24)
project(pkteditor LANGUAGES CXX)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Gui Test)
set(CMAKE_CXX_COMPILER /usr/sbin/clang++)
set(CMAKE_CXX_STANDARD 20)

set(PKT_SRC pmap.cpp transport.cpp)

add_executable(pktest test_main.cpp ${PKT_SRC})
add_executable(pkteditor gui_main.cpp upload.cpp pmap_dom.cpp mapper.cpp ${PKT_SRC})  # mapper.cpp = shared executor/preview, no copy (review rule)
add_executable(pktxc pktxc_main.cpp pktxc.cpp)
# LB5 upload engine + gate (own target: upload.cpp needs Qt6 Widgets, pktest must not)
add_executable(pktup upload_main.cpp upload.cpp pmap.cpp transport.cpp)
# Phase 0 (2026-11-12): lossless pmap DOM round-trip gate + lib for GUI/CLI
add_executable(pmaprt pmap_rt_main.cpp pmap_dom.cpp pmap.cpp)
target_include_directories(pmaprt PRIVATE /usr/include/qt6 /usr/include/qt6/QtCore)
target_link_libraries(pmaprt PRIVATE Qt6Core)

foreach(t IN ITEMS pktest pkteditor pktup)
  target_include_directories(${t} PRIVATE /usr/include/qt6 /usr/include/qt6/QtCore)
endforeach()
target_include_directories(pkteditor PRIVATE /usr/include/qt6/QtWidgets /usr/include/qt6/QtGui)
target_include_directories(pktup PRIVATE /usr/include/qt6/QtWidgets /usr/include/qt6/QtGui)

# hotspot: CMakeLists.txt — pktm mount added by the rules layer (own TU mapper.*;
# rules layer is pure Qt Core/Gui; --live additionally reuses transport.cpp, so
# pktm links USB+HID too — link line moved below find_library)
add_executable(pktm mapper_main.cpp mapper.cpp pmap.cpp transport.cpp)
target_include_directories(pktm PRIVATE /usr/include/qt6 /usr/include/qt6/QtCore /usr/include/qt6/QtNetwork)

find_library(USB_LIB NAMES usb-1.0)
set(HID_LIB /usr/lib/libhidapi-libusb.so)
find_library(XKB_LIB NAMES xkbcommon)
if(XKB_LIB)
add_compile_definitions(PKT_HAVE_XKB)
endif()
target_link_libraries(pktest PRIVATE Qt6Core "${USB_LIB}" "${HID_LIB}")
add_compile_definitions(pkteditor QT_WIDGETS_LIB)  # for QTest::mouseClick QWidget overloads (manual link, cmake feature macro normally set by qt_add_*)
target_link_libraries(pkteditor PRIVATE Qt6Core Qt6Gui Qt6Widgets Qt6Test "${USB_LIB}" "${HID_LIB}" "${XKB_LIB}")
target_link_libraries(pktup PRIVATE Qt6Core Qt6Gui Qt6Widgets "${USB_LIB}" "${HID_LIB}")
target_link_libraries(pktm PRIVATE Qt6Core Qt6Gui "${USB_LIB}" "${HID_LIB}" "${XKB_LIB}")
