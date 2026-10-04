#pragma once

#include <chrono>

#include "baton/types.h"

namespace baton {

// Injected everywhere time matters, so tests can control time instead of sleeping.
class Clock {
   public:
    virtual ~Clock() = default;
    virtual TimeMs now() const = 0;
};

class SystemClock final : public Clock {
   public:
    TimeMs now() const override {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }
};

class FakeClock final : public Clock {
   public:
    explicit FakeClock(TimeMs start = 1'000'000) : now_(start) {}
    TimeMs now() const override { return now_; }
    void advance(TimeMs ms) { now_ += ms; }
    void set(TimeMs t) { now_ = t; }

   private:
    TimeMs now_;
};

}  // namespace baton
