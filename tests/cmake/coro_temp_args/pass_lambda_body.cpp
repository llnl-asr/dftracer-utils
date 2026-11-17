// SPDX-License-Identifier: MIT
Task<int> run() {
    co_await f([&]() -> Task<int> {
        auto op = g(Person{"x", 1});
        co_return 0;
    });
    co_return 0;
}
