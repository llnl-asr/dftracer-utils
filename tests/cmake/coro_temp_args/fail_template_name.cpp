// SPDX-License-Identifier: MIT
Task<int> run() {
    co_await f(a, dstd::Vector<int>{1, 2});
    co_return 0;
}
