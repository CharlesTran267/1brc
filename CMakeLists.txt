project(charles_1brc)

find_package(OpenSSL REQUIRED)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

add_executable(charles_1brc src/main.cpp)
target_link_libraries(charles_1brc
  PRIVATE OpenSSL::Crypto
)
