// SPDX-License-Identifier: MIT
#pragma once
#include <string>
#include <vector>

namespace demo {

class Counter {
public:
    explicit Counter(std::string name) : name_(std::move(name)) {}
    int add(int v) { n_ += v; return n_; }
    int get() const { return n_; }
    const char* name() const { return name_.c_str(); }
    void reset() { n_ = 0; }

private:
    std::string name_;
    int n_ = 0;
};

} // namespace demo
