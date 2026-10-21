Task<int> run(int c) {
    co_await f([&]() -> Task<int> {
        auto h = [c] { return c; };
        co_return h();
    });
    co_return 0;
}
