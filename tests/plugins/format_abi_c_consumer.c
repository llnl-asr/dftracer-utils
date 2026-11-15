// SPDX-License-Identifier: MIT
/* A real C consumer of the dftu_svc_format ABI, for dftu.svc.json and
 * dftu.svc.yaml. The calls are synchronous, so the scenario runs them in
 * place. A YAML scalar is untyped text, so the YAML checks read the text where
 * the JSON checks read a number or a bool. */
#include <dftracer/utils/plugins/abi.h>
#include <plugins/abi_c_consumers.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MSG_CAP 256

typedef struct {
    dftu_ctx* ctx;
    const dftu_svc_format* fmt;
    const dftu_test_harness* hn;
    int is_yaml;
} scn;

static const char JSON_TEXT[] =
    "{\"name\":\"demo\",\"n\":42,\"pi\":2.5,\"ok\":true,\"none\":null,"
    "\"list\":[1,\"two\",{\"deep\":[10,20,30]}],"
    "\"nested\":{\"a\":{\"b\":\"found\"}}}";

static const char YAML_TEXT[] =
    "name: demo\n"
    "n: 42\n"
    "pi: 2.5\n"
    "ok: true\n"
    "none:\n"
    "list:\n"
    "  - 1\n"
    "  - two\n"
    "  - deep:\n"
    "      - 10\n"
    "      - 20\n"
    "      - 30\n"
    "nested:\n"
    "  a:\n"
    "    b: found\n";

static int is_text(const dftu_value* v, const char* text) {
    uint32_t len = 0;
    const char* s = dftu_as_str(v, &len);
    return s && len == strlen(text) && memcmp(s, text, len) == 0;
}

/* A scalar that reads back as `want` in the format: a number in JSON, its text
 * in YAML. */
static int is_int(const scn* s, const dftu_value* v, int64_t want,
                  const char* text) {
    if (s->is_yaml) return is_text(v, text);
    return v && v->kind == DFTU_VAL_I64 && v->as.i64 == want;
}

static int has_line_column(const char* message) {
    unsigned line = 0, column = 0;
    int used = 0;
    return sscanf(message, "%u:%u:%n", &line, &column, &used) == 2 &&
           line > 0 && column > 0 && used > 0;
}

static void last_message(const scn* s, char* msg) {
    msg[0] = 0;
    s->hn->ctx_message(s->ctx, msg, MSG_CAP);
}

static void check_document(scn* s) {
    const dftu_svc_format* f = s->fmt;
    const char* text = s->is_yaml ? YAML_TEXT : JSON_TEXT;
    dftu_doc* doc = NULL;
    s->hn->expect("parse the nested document",
                  f->parse(s->ctx, NULL, text, strlen(text), &doc) == DFTU_OK &&
                      doc != NULL);
    if (!doc) return;
    const dftu_value* root = f->root(doc);
    s->hn->expect("root is an object", root && root->kind == DFTU_VAL_OBJECT);
    s->hn->expect("root is built once", f->root(doc) == root);
    s->hn->expect("a string member",
                  is_text(dftu_obj_get(root, "name"), "demo"));
    s->hn->expect("an integer member",
                  is_int(s, dftu_obj_get(root, "n"), 42, "42"));
    if (!s->is_yaml) {
        s->hn->expect("a float member",
                      dftu_obj_get(root, "pi")->kind == DFTU_VAL_F64 &&
                          dftu_as_f64(dftu_obj_get(root, "pi"), 0) == 2.5);
        s->hn->expect("a bool member",
                      dftu_as_bool(dftu_obj_get(root, "ok"), 0));
    } else {
        s->hn->expect("a float member as text",
                      is_text(dftu_obj_get(root, "pi"), "2.5"));
    }
    s->hn->expect("a null member",
                  dftu_obj_get(root, "none") &&
                      dftu_obj_get(root, "none")->kind == DFTU_VAL_NULL);
    s->hn->expect("a missing member", dftu_obj_get(root, "absent") == NULL);

    const dftu_value* nested = dftu_obj_get(root, "nested");
    const dftu_value* b = dftu_obj_get(dftu_obj_get(nested, "a"), "b");
    s->hn->expect("a nested key", is_text(b, "found"));

    const dftu_value* list = dftu_obj_get(root, "list");
    s->hn->expect("an array of three",
                  list && list->kind == DFTU_VAL_ARRAY && list->count == 3);
    if (list && list->count == 3) {
        s->hn->expect("array element 0", is_int(s, &list->as.items[0], 1, "1"));
        s->hn->expect("array element 1", is_text(&list->as.items[1], "two"));
        const dftu_value* deep = dftu_obj_get(&list->as.items[2], "deep");
        s->hn->expect("an array in an array element",
                      deep && deep->kind == DFTU_VAL_ARRAY && deep->count == 3);
        if (deep && deep->count == 3)
            s->hn->expect("a nested array element",
                          is_int(s, &deep->as.items[2], 30, "30"));
    }
    f->doc_free(doc);
}

static dftu_status ok(scn* s, dftu_status st);

static void check_wide_and_nul(scn* s) {
    const dftu_svc_format* f = s->fmt;
    dftu_doc* doc = NULL;
    const char* text = s->is_yaml
                           ? "u: 18446744073709551615\ns: \"a\\0b\"\n"
                           : "{\"u\":18446744073709551615,\"s\":\"a\\u0000b\"}";
    s->hn->expect("parse a wide integer and an embedded NUL",
                  f->parse(s->ctx, NULL, text, strlen(text), &doc) == DFTU_OK);
    if (!doc) return;
    const dftu_value* root = f->root(doc);
    const dftu_value* u = dftu_obj_get(root, "u");
    if (s->is_yaml)
        s->hn->expect("a wide integer is text in YAML",
                      is_text(u, "18446744073709551615"));
    else
        s->hn->expect("a uint64 above INT64_MAX is F64",
                      u && u->kind == DFTU_VAL_F64 &&
                          u->as.f64 == 18446744073709551615.0);
    uint32_t len = 0;
    const char* str = dftu_as_str(dftu_obj_get(root, "s"), &len);
    s->hn->expect(
        "an embedded NUL stays in the string and in its count",
        str && len == 3 && memcmp(str, "a\0b", 3) == 0 && str[3] == 0);
    f->doc_free(doc);

    dftu_format_writer* w = NULL;
    f->writer_new(s->ctx, NULL, &w);
    dftu_bytes out = {NULL, 0, NULL, NULL};
    if (!w) return;
    ok(s, f->begin_object(s->ctx, w));
    ok(s, f->key(s->ctx, w, "k", 1));
    ok(s, f->str(s->ctx, w, "x\0y", 3));
    ok(s, f->end_object(s->ctx, w));
    ok(s, f->finish(s->ctx, w, &out));
    doc = NULL;
    s->hn->expect("a string with a NUL parses back",
                  f->parse(s->ctx, NULL, (const char*)out.data, out.len,
                           &doc) == DFTU_OK);
    if (doc) {
        str = dftu_as_str(dftu_obj_get(f->root(doc), "k"), &len);
        s->hn->expect("the NUL survives the round trip",
                      str && len == 3 && memcmp(str, "x\0y", 3) == 0);
        f->doc_free(doc);
    }
    if (out.free_fn) out.free_fn((void*)(uintptr_t)out.data, out.ud);
    f->writer_free(w);
}

static void check_parse_error(scn* s, const char* text, const char* at) {
    const dftu_svc_format* f = s->fmt;
    char msg[MSG_CAP];
    dftu_doc* doc = (dftu_doc*)(uintptr_t)1;
    const dftu_status st = f->parse(s->ctx, NULL, text, strlen(text), &doc);
    last_message(s, msg);
    s->hn->expect("a bad text fails with a parse status", st == DFTU_E_PARSE);
    s->hn->expect("a failed parse leaves no document", doc == NULL);
    s->hn->expect("the message has a line and a column", has_line_column(msg));
    s->hn->expect("the message names the place", strstr(msg, at) == msg);
}

static void check_parse_errors(scn* s) {
    if (s->is_yaml) {
        check_parse_error(s, "a: 1\nb: 2\na: 3\n", "3:1:");
        check_parse_error(s, "a: 1\nb: [1, 2\n", "2:4:");
    } else {
        check_parse_error(s, "{\"a\": [1, 2,\n  oops]}", "2:3:");
        check_parse_error(s, "{\"a\": 1,}", "1:9:");
        check_parse_error(s, "", "1:1:");
        check_parse_error(s, "7", "1:1:");
        check_parse_error(s, "{\"a\": 1} x", "1:10:");
        check_parse_error(s, "{\"a\": 123456789012345678901}", "1:7:");
        check_parse_error(s, "{\"a\": \"\xff\"}", "1:8:");
        check_parse_error(s, "{\"a\": 1e999}", "1:7:");
    }
}

static dftu_status ok(scn* s, dftu_status st) {
    s->hn->expect("a writer call succeeds", st == DFTU_OK);
    return st;
}

static void write_sample(scn* s, dftu_format_writer* w) {
    const dftu_svc_format* f = s->fmt;
    dftu_ctx* c = s->ctx;
    const char tricky[] = "he said \"hi\"\nthen left";
    ok(s, f->begin_object(c, w));
    ok(s, f->key(c, w, "s", 1));
    ok(s, f->str(c, w, tricky, strlen(tricky)));
    ok(s, f->key(c, w, "i", 1));
    ok(s, f->i64(c, w, -7));
    ok(s, f->key(c, w, "f", 1));
    ok(s, f->f64(c, w, 1.5));
    ok(s, f->key(c, w, "b", 1));
    ok(s, f->boolean(c, w, 1));
    ok(s, f->key(c, w, "n", 1));
    ok(s, f->null(c, w));
    ok(s, f->key(c, w, "r", 1));
    ok(s, f->raw(c, w, "12", 2));
    ok(s, f->key(c, w, "arr", 3));
    ok(s, f->begin_array(c, w));
    ok(s, f->i64(c, w, 1));
    ok(s, f->i64(c, w, 2));
    ok(s, f->begin_object(c, w));
    ok(s, f->key(c, w, "x", 1));
    ok(s, f->str(c, w, "y", 1));
    ok(s, f->end_object(c, w));
    ok(s, f->end_array(c, w));
    ok(s, f->key(c, w, "empty_o", 7));
    ok(s, f->begin_object(c, w));
    ok(s, f->end_object(c, w));
    ok(s, f->key(c, w, "empty_a", 7));
    ok(s, f->begin_array(c, w));
    ok(s, f->end_array(c, w));
    ok(s, f->end_object(c, w));
}

static void check_round_trip(scn* s) {
    const dftu_svc_format* f = s->fmt;
    dftu_format_writer* w = NULL;
    s->hn->expect("a new writer",
                  f->writer_new(s->ctx, NULL, &w) == DFTU_OK && w != NULL);
    if (!w) return;
    write_sample(s, w);
    dftu_bytes out = {NULL, 0, NULL, NULL};
    s->hn->expect("finish", f->finish(s->ctx, w, &out) == DFTU_OK);
    s->hn->expect("the text has a release function",
                  out.free_fn != NULL && out.len > 0);

    dftu_doc* doc = NULL;
    s->hn->expect("the text parses back",
                  f->parse(s->ctx, NULL, (const char*)out.data, out.len,
                           &doc) == DFTU_OK);
    if (doc) {
        const dftu_value* root = f->root(doc);
        s->hn->expect(
            "round trip string",
            is_text(dftu_obj_get(root, "s"), "he said \"hi\"\nthen left"));
        s->hn->expect("round trip integer",
                      is_int(s, dftu_obj_get(root, "i"), -7, "-7"));
        s->hn->expect("round trip raw",
                      is_int(s, dftu_obj_get(root, "r"), 12, "12"));
        if (!s->is_yaml) {
            const dftu_value* fl = dftu_obj_get(root, "f");
            s->hn->expect("round trip float",
                          fl && fl->kind == DFTU_VAL_F64 && fl->as.f64 == 1.5);
            const dftu_value* bl = dftu_obj_get(root, "b");
            s->hn->expect("round trip bool",
                          bl && bl->kind == DFTU_VAL_BOOL && bl->as.b == 1);
        } else {
            s->hn->expect("round trip float as text",
                          is_text(dftu_obj_get(root, "f"), "1.5"));
            s->hn->expect("round trip bool as text",
                          is_text(dftu_obj_get(root, "b"), "true"));
        }
        const dftu_value* nv = dftu_obj_get(root, "n");
        s->hn->expect("round trip null", nv && nv->kind == DFTU_VAL_NULL);
        const dftu_value* arr = dftu_obj_get(root, "arr");
        s->hn->expect("round trip array",
                      arr && arr->kind == DFTU_VAL_ARRAY && arr->count == 3 &&
                          is_int(s, &arr->as.items[1], 2, "2") &&
                          is_text(dftu_obj_get(&arr->as.items[2], "x"), "y"));
        const dftu_value* eo = dftu_obj_get(root, "empty_o");
        const dftu_value* ea = dftu_obj_get(root, "empty_a");
        s->hn->expect("round trip empty object",
                      eo && eo->kind == DFTU_VAL_OBJECT && eo->count == 0);
        s->hn->expect("round trip empty array",
                      ea && ea->kind == DFTU_VAL_ARRAY && ea->count == 0);
        f->doc_free(doc);
    }
    out.free_fn((void*)(uintptr_t)out.data, out.ud);
    s->hn->expect("a finished writer takes no more calls",
                  f->i64(s->ctx, w, 1) == DFTU_E_INVALID);
    f->writer_free(w);
}

static void expect_invalid(scn* s, const char* what, dftu_status st) {
    char msg[MSG_CAP];
    s->hn->expect(what, st == DFTU_E_INVALID);
    last_message(s, msg);
    s->hn->expect("a refusal sets a message", msg[0] != 0);
}

static void check_misuse(scn* s) {
    const dftu_svc_format* f = s->fmt;
    dftu_ctx* c = s->ctx;
    dftu_bytes none = {NULL, 0, NULL, NULL};
    dftu_format_writer* w = NULL;
    f->writer_new(c, NULL, &w);
    if (!w) return;
    expect_invalid(s, "end with nothing open", f->end_object(c, w));
    expect_invalid(s, "key outside an object", f->key(c, w, "k", 1));
    expect_invalid(s, "finish with nothing written", f->finish(c, w, &none));
    ok(s, f->i64(c, w, 5));
    expect_invalid(s, "a second root", f->i64(c, w, 6));
    dftu_bytes out = {NULL, 0, NULL, NULL};
    ok(s, f->finish(c, w, &out));
    if (out.free_fn) out.free_fn((void*)(uintptr_t)out.data, out.ud);
    f->writer_free(w);

    w = NULL;
    f->writer_new(c, NULL, &w);
    if (!w) return;
    ok(s, f->begin_object(c, w));
    expect_invalid(s, "a value without a key", f->i64(c, w, 1));
    expect_invalid(s, "an array without a key", f->begin_array(c, w));
    ok(s, f->key(c, w, "a", 1));
    expect_invalid(s, "two keys in a row", f->key(c, w, "b", 1));
    expect_invalid(s, "end with a key pending", f->end_object(c, w));
    expect_invalid(s, "end of the other kind", f->end_array(c, w));
    ok(s, f->null(c, w));
    expect_invalid(s, "finish with a container open", f->finish(c, w, &none));
    expect_invalid(s, "an empty raw token", f->raw(c, w, "", 0));
    f->key(c, w, "last", 4);
    f->writer_free(w);

    w = NULL;
    f->writer_new(c, NULL, &w);
    if (!w) return;
    volatile double zero = 0.0;
    const dftu_status nan_status = f->f64(c, w, zero / zero);
    if (s->is_yaml)
        s->hn->expect("YAML writes a double that is not finite",
                      nan_status == DFTU_OK);
    else
        expect_invalid(s, "JSON refuses a double that is not finite",
                       nan_status);
    f->writer_free(w);
}

static void check_depth(scn* s) {
    const dftu_svc_format* f = s->fmt;
    char text[4096];
    char msg[MSG_CAP];
    const int depth = 1002;
    int n = 0;
    for (int i = 0; i < depth; ++i) text[n++] = '[';
    for (int i = 0; i < depth; ++i) text[n++] = ']';
    text[n] = 0;
    dftu_doc* doc = (dftu_doc*)(uintptr_t)1;
    const dftu_status st = f->parse(s->ctx, NULL, text, (uint64_t)n, &doc);
    last_message(s, msg);
    s->hn->expect("nesting past the limit is refused with a parse status",
                  st == DFTU_E_PARSE && doc == NULL && msg[0] != 0);
}

static void check_long_key(scn* s) {
    const dftu_svc_format* f = s->fmt;
    char key[1200];
    memset(key, 'k', sizeof key);
    dftu_format_writer* w = NULL;
    f->writer_new(s->ctx, NULL, &w);
    if (!w) return;
    ok(s, f->begin_object(s->ctx, w));
    const dftu_status st = f->key(s->ctx, w, key, sizeof key);
    if (s->is_yaml)
        expect_invalid(s, "a YAML key over 1022 bytes is refused", st);
    else
        s->hn->expect("a long JSON key is written", st == DFTU_OK);
    f->writer_free(w);
}

static void check_free_null(scn* s) {
    s->fmt->doc_free(NULL);
    s->fmt->writer_free(NULL);
    s->hn->expect("the root of no document is NULL",
                  s->fmt->root(NULL) == NULL);
}

void dftu_test_format_ops(dftu_ctx* ctx, const dftu_plugin_host* h,
                          const dftu_test_harness* hn, const char* id) {
    scn sc = {ctx, (const dftu_svc_format*)h->get_service(h->h, id), hn,
              strcmp(id, DFTU_SVC_YAML) == 0};
    scn* s = &sc;
    hn->expect("the service is served", s->fmt != NULL);
    if (!s->fmt) return;
    hn->expect("the table has its size",
               s->fmt->struct_size == sizeof(dftu_svc_format));
    check_document(s);
    check_parse_errors(s);
    check_wide_and_nul(s);
    check_depth(s);
    check_round_trip(s);
    check_misuse(s);
    check_long_key(s);
    check_free_null(s);
}
