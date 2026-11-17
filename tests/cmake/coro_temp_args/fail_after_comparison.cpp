// SPDX-License-Identifier: MIT
Task<int> run(int i, int n) {
    co_await f(i < n, Foo{});
    co_return 0;
}
