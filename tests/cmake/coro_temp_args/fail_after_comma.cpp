// SPDX-License-Identifier: MIT
Task<int> run() {
    co_await group.send(rank, dstd::String{"x"}, Codec<int, long>{});
    co_return 0;
}
