cmake_minimum_required(VERSION 3.20)

project(amscope-app VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
find_package(OpenCV 5 REQUIRED COMPONENTS core imgcodecs imgproc videoio)

add_executable(amscope-app src/amscope-app.cpp)

target_compile_options(amscope-app PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(amscope-app PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(amscope-app PRIVATE PkgConfig::GTK3 ${OpenCV_LIBS})

install(TARGETS amscope-app RUNTIME DESTINATION bin)
install(FILES amscope-microscope.desktop
        DESTINATION share/applications
        RENAME com.skot.AmScope.desktop)
