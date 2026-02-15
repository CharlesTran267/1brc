project(charles_1brc)

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

if(DEFINED NUM_THREADS)
  target_compile_definitions(charles_1brc PRIVATE NUM_THREADS=${NUM_THREADS})
endif()
