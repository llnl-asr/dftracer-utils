// SPDX-License-Identifier: MIT
Task<int> run(int a, int b, int c) {
    co_await f(a < b, c);
    co_await f(a<b, c> a, g(c));
    co_return 0;
}
