Task<int> run(int c) {
    co_await f([&]() -> Task<int> {
        co_await g([c] { return c; });
        co_return 0;
    });
    co_return 0;
}
