add_library(job_system
    scheduler.cpp
    thread_pool.cpp
    wrr_policy.cpp
    drr_policy.cpp
)

target_include_directories(job_system PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(job_system PUBLIC Threads::Threads)
