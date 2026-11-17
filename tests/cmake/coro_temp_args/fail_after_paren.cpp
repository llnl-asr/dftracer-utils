// SPDX-License-Identifier: MIT
Task<int> run() {
    auto r = co_await group.gather(Person{"x", 1});
    co_return r;
}
