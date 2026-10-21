Task<int> run(int c) {
    int x = 0;
    co_await f([&]() -> Task<int> { co_return 1; });
    co_await f([&x]() -> Task<int> { co_return x; });
    co_await f([this, &x]() -> Task<int> { co_return x; });
    auto named = [c]() -> Task<int> { co_return c; };
    co_await f(named);
    co_return 0;
}
