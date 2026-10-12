# Builder support: reading the player's disc (and, later, the guided builder app).
add_library(ps2x_builder STATIC src/ps2x_iso9660.cpp)
target_include_directories(ps2x_builder PUBLIC include)
target_compile_features(ps2x_builder PUBLIC cxx_std_20)

add_executable(ps2x_iso tools/ps2x_iso.cpp)
target_link_libraries(ps2x_iso PRIVATE ps2x_builder)
FetchContent_GetProperties(xxhash)
if(xxhash_SOURCE_DIR)
    target_include_directories(ps2x_iso PRIVATE "${xxhash_SOURCE_DIR}")
endif()
