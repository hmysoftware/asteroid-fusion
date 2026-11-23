cmake_minimum_required(VERSION 3.16)
project(AFX C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O2")

# ---- raylib (desktop / GLFW) ----
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(PLATFORM Desktop CACHE STRING "" FORCE)
add_subdirectory(vendor/raylib EXCLUDE_FROM_ALL)

# ---- Lua 5.4 ----
file(GLOB LUA_SRC ${CMAKE_SOURCE_DIR}/vendor/lua/*.c)
list(FILTER LUA_SRC EXCLUDE REGEX "(ltests|lua|luac)\\.c$")
add_library(lua54 STATIC ${LUA_SRC})
target_include_directories(lua54 PUBLIC vendor/lua)
target_compile_definitions(lua54 PRIVATE LUA_USE_POSIX)
target_compile_options(lua54 PRIVATE -w)

# ---- AFX ----
file(GLOB AFX_SRC ${CMAKE_SOURCE_DIR}/src/*.c)
add_executable(afx ${AFX_SRC})
target_include_directories(afx PRIVATE
  src vendor/lua vendor/nanosvg vendor/stb
  vendor/raylib/src)
target_compile_definitions(afx PRIVATE
  AFX_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
target_compile_options(afx PRIVATE -Wall -Wno-unused-function -Wno-unused-variable)
target_link_libraries(afx PRIVATE raylib lua54 m)
