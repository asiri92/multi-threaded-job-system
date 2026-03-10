#pragma once
#include <chrono>
#include <cstdint>
#include <string>

namespace job_system {

class IMetricsObserver {
public:
    virtual ~IMetricsObserver() = default;
    virtual void on_job_submitted(const std::string& /*client_id*/, uint64_t /*job_id*/) {}
    virtual void on_job_executed(const std::string& /*client_id*/, uint64_t /*job_id*/,
                                  std::chrono::microseconds /*duration*/) {}
    virtual void on_job_expired(const std::string& /*client_id*/, uint64_t /*job_id*/) {}
    virtual void on_job_cancelled(const std::string& /*client_id*/, uint64_t /*job_id*/) {}
};

} // namespace job_system
