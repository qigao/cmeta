/* Compile with -std=c11 -Wall -Wextra -Werror -pedantic on Clang/GCC.
 * No C23 omitted-variadic-argument extension is needed for one-row relations.
 */
#include <cmeta/compute.h>

#define PP_MAP_ADD(item_, context_) + (item_)
#define PP_MAP_VALUE(item_, context_) (item_)

enum {
    map_sum_three = 0 CMETA_PP_MAP(PP_MAP_ADD, ~, 1, 2, 3),
    map_sum_sixteen = 0 CMETA_PP_MAP(PP_MAP_ADD, ~,
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16),
    map_zero_is_empty = 7 CMETA_PP_MAP_N(0, PP_MAP_ADD, ~,)
};

static const int map_values[] = {
    CMETA_PP_MAP_COMMA(PP_MAP_VALUE, ~, 4, 5, 6)
};

_Static_assert(map_sum_three == 6, "inferred map expands each item once");
_Static_assert(map_sum_sixteen == 136, "finite map supports the documented 16-item bound");
_Static_assert(map_zero_is_empty == 7, "explicit zero-item map emits no tokens");
_Static_assert(sizeof(map_values) / sizeof(map_values[0]) == 3,
               "comma map emits exactly one element per input");
_Static_assert(map_values[0] == 4 && map_values[1] == 5 && map_values[2] == 6,
               "comma map preserves input order");

enum { first_single = CMETA_PP_FIRST(7) };
enum { first_many = CMETA_PP_FIRST(3, 5, 7) };

TypeFunction(PPOnly, (int, double, double));
TypeFunction(PPMulti, (int, double, double), (short, double, float));

typedef TypeEval(PPOnly, int, double) pp_only_result;
typedef TypeEval(PPMulti, short, double) pp_multi_result;

_Static_assert(first_single == 7, "single item FIRST");
_Static_assert(first_many == 3, "many items FIRST");
_Static_assert(sizeof(pp_only_result) == sizeof(double), "one-row TypeFunction");
_Static_assert(sizeof(pp_multi_result) == sizeof(float), "multi-row TypeFunction");

int main(void)
{
    return 0;
}
