// SPDX-License-Identifier: MIT
Task<int> run() {
    auto op = group.gather(Person{"x", 1});
    auto r = co_await op;
    Person p{"y", 2};
    co_await group.gather(p);
    co_return r;
}
