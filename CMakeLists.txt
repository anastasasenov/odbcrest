cmake_minimum_required(VERSION 3.15)
project(odbcrest VERSION 1.0.0 LANGUAGES C)

# C11 
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wformat=2 -O2)
endif()

if(DEFINED ODBCREST_LOG)
    add_definitions(-DODBCREST_LOG=${ODBCREST_LOG})
endif()

# shared object
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# dependencies
find_package(ODBC REQUIRED)
find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)
find_library(ODBC_INST_LIBRARY NAMES odbcinst)

# sources
add_library(odbcrest SHARED odbcrest.c)

# includes
target_include_directories(odbcrest PRIVATE 
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CURL_INCLUDE_DIRS}
)

# libs
target_link_libraries(odbcrest
    PRIVATE
        ODBC::ODBC
        CURL::libcurl
        PkgConfig::JSONC
        ${ODBC_INST_LIBRARY}
)

target_compile_definitions(odbcrest PRIVATE odbcrest_EXPORTS)

# install
install(TARGETS odbcrest
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} # .so
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} # .dll
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} # .lib
)

message(STATUS "CURL: ${CURL_LIBRARIES}")
message(STATUS "JSON-C: ${JSONC_LIBRARIES}")
message(STATUS "ODBCINST: ${ODBC_INST_LIBRARY}")
message(STATUS "ODBC REST Driver configuraion done.")
