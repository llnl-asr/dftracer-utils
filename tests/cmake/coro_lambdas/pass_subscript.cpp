Task<int> run(int *a, int i) {
    co_await f(a[i]);
    co_await f(a[i], b[i + 1]);
    co_return 0;
}
