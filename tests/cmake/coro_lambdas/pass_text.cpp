// co_await f([x] { return x; });
/* co_await f([x]
   { return x; }); */
Task<int> run(int x) {
    const char *s = "co_await f([x] { return x; });";
    co_await g();
    auto h = [x] { return x; };
    co_return h();
}
