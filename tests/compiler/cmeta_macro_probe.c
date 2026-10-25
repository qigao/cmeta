/* Exercise public CMeta declaration syntax, not only the linked library ABI. */
#include <cmeta/meta.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

cmeta_enum(compiler_probe_color,
    (COMPILER_RED, "red"),
    (COMPILER_BLUE, "blue")
);

cmeta_struct(compiler_probe_record,
    cmeta_field(int, id)
    cmeta_field(double, weight)
);

cmeta_require_field(compiler_probe_record, id, int);
cmeta_require_field(compiler_probe_record, weight, double);

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "CMeta declaration macro failed at %d: %s\n", __LINE__, #expr); \
    return 1; \
} } while (0)

int main(void) {
    compiler_probe_record record = {17, 3.5};
    compiler_probe_color color = COMPILER_RED;
    const cmeta_struct_desc *s = compiler_probe_record_meta();
    const cmeta_enum_desc *e = compiler_probe_color_meta();
    const cmeta_field_desc *f;

    CHECK(s != NULL && s->size == sizeof(record));
    CHECK(s->align == _Alignof(compiler_probe_record));
    CHECK(s->field_count == 2u);
    f = cmeta_struct_find_field(s, "id");
    CHECK(f != NULL && f->offset == offsetof(compiler_probe_record, id));
    CHECK(f->size == sizeof(record.id));
    f = cmeta_struct_find_field(s, "weight");
    CHECK(f != NULL && f->offset == offsetof(compiler_probe_record, weight));
    CHECK(f->size == sizeof(record.weight));
    CHECK(cmeta_struct_find_field(s, "missing") == NULL);
    CHECK(record.id == 17 && record.weight == 3.5);

    CHECK(e != NULL && e->count == 2u);
    CHECK(strcmp(compiler_probe_color_to_string(COMPILER_BLUE), "blue") == 0);
    CHECK(compiler_probe_color_from_string("blue", &color));
    CHECK(color == COMPILER_BLUE);
    CHECK(!compiler_probe_color_from_string("missing", &color));
    CHECK(color == COMPILER_BLUE);
    puts("CMeta public enum/struct declaration macros: PASS");
    return 0;
}
