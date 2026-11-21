cmake_minimum_required(VERSION 3.21)
project(ChatBot)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PROJECT_ROOT ${CMAKE_SOURCE_DIR})
set(INCLUDE_DIR ${PROJECT_ROOT}/include)

include_directories(${INCLUDE_DIR})

if(WIN32)
    add_compile_definitions(WIN32_WINNT=0x0A00 WINVER=0x0A00)
    add_compile_definitions(_WIN32_WINNT_WIN10=0x0A00)
endif()

add_compile_definitions(CPPHTTPLIB_OPENSSL_SUPPORT=1)

# Some Windows OpenSSL installers keep import libraries under lib/VC/x64/MD.
# Resolve that layout relative to the user-selected root, without a drive path.
if(WIN32 AND NOT OPENSSL_ROOT_DIR AND NOT "$ENV{OPENSSL_ROOT_DIR}" STREQUAL "")
    set(OPENSSL_ROOT_DIR "$ENV{OPENSSL_ROOT_DIR}")
endif()
if(WIN32 AND OPENSSL_ROOT_DIR)
    find_library(SSL_EAY_RELEASE NAMES libssl ssl
        PATHS "${OPENSSL_ROOT_DIR}"
        PATH_SUFFIXES lib/VC/x64/MD lib/VC/x64/MT lib NO_DEFAULT_PATH)
    find_library(LIB_EAY_RELEASE NAMES libcrypto crypto
        PATHS "${OPENSSL_ROOT_DIR}"
        PATH_SUFFIXES lib/VC/x64/MD lib/VC/x64/MT lib NO_DEFAULT_PATH)
endif()

find_package(OpenSSL REQUIRED)
find_package(hiredis CONFIG REQUIRED)
find_package(PostgreSQL REQUIRED)

# Find the runtime DLLs next to the selected OpenSSL installation, if dynamic.
if(WIN32)
    get_filename_component(OPENSSL_INSTALL_PREFIX "${OPENSSL_INCLUDE_DIR}" DIRECTORY)
    file(GLOB OPENSSL_RUNTIME_DLLS LIST_DIRECTORIES false
        "${OPENSSL_INSTALL_PREFIX}/bin/libssl*.dll"
        "${OPENSSL_INSTALL_PREFIX}/bin/libcrypto*.dll")
endif()

set(SOURCES
    src/main.cpp
    src/SQLiteDB.cpp
    src/Conversation.cpp
    src/utils.cpp
    src/RedisClient.cpp
)

if(MSVC)
    add_compile_options(/utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()

# 创建目标
add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${PostgreSQL_INCLUDE_DIRS}
)

# 链接库
target_link_libraries(${PROJECT_NAME} PRIVATE
    OpenSSL::SSL
    OpenSSL::Crypto
    hiredis::hiredis
    ${PostgreSQL_LIBRARIES}
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ws2_32 crypt32 user32 advapi32 bcrypt winhttp
    )
endif()

if(UNIX AND NOT APPLE)
    target_link_libraries(${PROJECT_NAME} PRIVATE pthread)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_ROOT}/Clone and TTS.py"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_ROOT}/src/index.html"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_ROOT}/Caddyfile.example"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_ROOT}/config.runtime.example.bat"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_ROOT}/include/libpq.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_ROOT}/include/libcrypto-3-x64.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_ROOT}/include/libintl-9.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_ROOT}/include/libssl-3-x64.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_ROOT}/include/hiredis.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_ROOT}/include/libwinpthread-1.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_ROOT}/include/libiconv-2.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_ROOT}/certs/cacert.pem"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${PROJECT_ROOT}/start in ECS.bat"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    COMMENT "拷贝运行资源和随项目提供的依赖 DLL 到输出目录"
)

# Copy the DLLs matching the OpenSSL installation used for linking last, so
# bundled compatibility DLLs cannot overwrite the selected runtime version.
foreach(OPENSSL_RUNTIME_DLL IN LISTS OPENSSL_RUNTIME_DLLS)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${OPENSSL_RUNTIME_DLL}" "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    )
endforeach()
