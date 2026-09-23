/* The DFTU_SCALAR_* macros are C-only (guarded by #ifndef __cplusplus in
 * abi.h) since they use C99 designated initializers on a union member no
 * C++ literal syntax reaches the same way. Exercised from a plain C TU so
 * the designated-initializer form actually compiles and runs. */
#include <assert.h>
#include <dftracer/utils/dataframe/abi.h>
#include <string.h>

int main(void) {
    dftu_scalar i = DFTU_SCALAR_I64(5);
    assert(i.kind == DFTU_SCALAR_TAG_I64);
    assert(i.len == 0);
    assert(i.value.i == 5);

    dftu_scalar u = DFTU_SCALAR_U64(7);
    assert(u.kind == DFTU_SCALAR_TAG_U64);
    assert(u.len == 0);
    assert(u.value.u == 7);

    dftu_scalar f = DFTU_SCALAR_F64(2.5);
    assert(f.kind == DFTU_SCALAR_TAG_F64);
    assert(f.len == 0);
    assert(f.value.d == 2.5);

    const char* text = "POSIX";
    dftu_scalar s = DFTU_SCALAR_STR(text, 5);
    assert(s.kind == DFTU_SCALAR_TAG_STR);
    assert(s.len == 5);
    assert(memcmp(s.value.s, "POSIX", 5) == 0);

    return 0;
}
