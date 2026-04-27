#include "test.h"

#include <string.h>
#include <time.h>

#define TESTS(X)                                  \
    X(test_sim_semantics)                         \
    X(test_crc_vector)                            \
    X(test_basic_put_get)                         \
    X(test_edge_values)                           \
    X(test_identical_put_is_free)                 \
    X(test_foreach)                               \
    X(test_config_validation)                     \
    X(test_unformatted)                           \
    X(test_format_keeps_wear)                     \
    X(test_tombstones_reclaimed)                  \
    X(test_index_full)                            \
    X(test_fill_to_capacity)                      \
    X(test_bitrot_in_value)                       \
    X(test_bitrot_falls_back_to_older_version)    \
    X(test_bitrot_in_header)                      \
    X(test_damaged_block_header)                  \
    X(test_dynamic_wear_leveling)                 \
    X(test_static_wear_leveling)                  \
    X(test_model_geometries)                      \
    X(test_model_many_seeds)                      \
    X(test_powerloss_every_point)                 \
    X(test_powerloss_nested)                      \
    X(test_powerloss_random_long)

#define DECLARE(fn) void fn(void);
TESTS(DECLARE)

#define ENTRY(fn) {#fn, fn},
static const struct {
    const char *name;
    void (*fn)(void);
} tests[] = {TESTS(ENTRY)};

int main(int argc, char **argv)
{
    const char *filter = argc > 1 ? argv[1] : NULL;
    int ran = 0;
    clock_t all = clock();

    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        if (filter && !strstr(tests[i].name, filter))
            continue;
        t_current = tests[i].name;
        printf("[ RUN  ] %s\n", tests[i].name);
        fflush(stdout);
        clock_t t0 = clock();
        tests[i].fn();
        printf("[   OK ] %s (%.0f ms)\n", tests[i].name,
               1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC);
        ran++;
    }
    printf("\n%d tests passed in %.1f s\n", ran, (double)(clock() - all) / CLOCKS_PER_SEC);
    return ran ? 0 : 1;
}
