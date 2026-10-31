Task<int> run(int c) {
    auto r = co_await f(a, [c]() -> Task<int> {
        int x = c + 1;
        co_return x;
    });
    co_return r;
}
