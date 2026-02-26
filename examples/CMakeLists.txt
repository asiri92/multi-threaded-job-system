add_executable(basic_demo basic_demo.cpp)
target_link_libraries(basic_demo PRIVATE job_system)

add_executable(weighted_demo weighted_demo.cpp)
target_link_libraries(weighted_demo PRIVATE job_system)
