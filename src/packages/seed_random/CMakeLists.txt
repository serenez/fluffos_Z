# Seed Random Package
# Xoroshiro128++ 种子系统包

add_library(package_seed_random STATIC
        seed_random.cc
        )

target_include_directories(package_seed_random PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
        ${CMAKE_CURRENT_SOURCE_DIR}/../base
        )
