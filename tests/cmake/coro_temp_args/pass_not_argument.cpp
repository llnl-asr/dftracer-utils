// SPDX-License-Identifier: MIT
template <class T>
    requires requires(T t) { co_await t; }
Task<int> run(T t) {
    co_await t.f(1, 2);
    co_await t.f(a<b, c> d);
    co_await t.f(a, [](int x) { return Person{"x", x}; });
    co_await t.f(a, std::move(b));
    co_return 0;
}
