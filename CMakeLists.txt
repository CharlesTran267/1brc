project(charles_1brc)

# bench.sh reconfigures this dir; make sure a fresh cache never falls back to -O0
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-march=native)

find_package(OpenSSL REQUIRED)
find_package(absl REQUIRED)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)
# set(CMAKE_CXX_COMPILER clang++-20)
# add_compile_options(-stdlib=libc++ -fno-omit-frame-pointer -ggdb3)
# add_link_options(-stdlib=libc++)

add_executable(charles_1brc src/main.cpp)
target_link_libraries(charles_1brc
  PRIVATE 
  OpenSSL::Crypto
  absl::flat_hash_map
  absl::btree
)

foreach(v NUM_THREADS CONTAINER_TYPE MAP_SZ BATCH)
  if(DEFINED ${v})
    target_compile_definitions(charles_1brc PRIVATE ${v}=${${v}})
  endif()
endforeach()

# not in ALL: bench.sh's from-scratch build stays fast.
# run: cmake --build build --target hash_test && ./build/hash_test
add_executable(hash_test tests/hash_test.cpp)
target_link_libraries(hash_test PRIVATE OpenSSL::Crypto)
