// SPDX-License-Identifier: MIT
// co_await f(Person{"x", 1});
/* co_await f(a,
   Person{"x", 1}); */
Task<int> run() {
    const char *s = "co_await f(Person{1});";
    co_await g(s, "x{");
    auto h = f(Person{"x", 1});
    co_return 0;
}
