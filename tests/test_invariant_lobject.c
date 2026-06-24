#include <check.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* Simulate the vulnerable function from lobject.c */
static void vulnerable_pointer_format(char *buff, void *ptr) {
    /* Exact vulnerable code from lobject.c */
    sprintf(buff, "%p", ptr);
}

START_TEST(test_pointer_format_buffer_bounds)
{
    /* Invariant: Pointer representation must not exceed buffer capacity */
    void *payloads[] = {
        NULL,                           /* Valid: null pointer */
        (void *)0x1,                    /* Boundary: minimal non-null */
        (void *)0xFFFFFFFFFFFFFFFFULL,  /* Attack: maximum pointer value */
        (void *)0xDEADBEEFDEADBEEFULL,  /* Attack: typical exploit payload */
        (void *)0x7FFFFFFFFFFFFFFFULL   /* Boundary: max signed pointer */
    };
    int num_payloads = sizeof(payloads) / sizeof(payloads[0]);
    
    for (int i = 0; i < num_payloads; i++) {
        char buff[4 * sizeof(void *) + 8]; /* Same buffer size as original */
        size_t buffer_capacity = sizeof(buff);
        
        vulnerable_pointer_format(buff, payloads[i]);
        size_t written_length = strlen(buff) + 1; /* Include null terminator */
        
        /* Security property: written data must fit in buffer */
        ck_assert_msg(written_length <= buffer_capacity,
                     "Pointer %p caused buffer overflow (wrote %zu bytes, capacity %zu)",
                     payloads[i], written_length, buffer_capacity);
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_pointer_format_buffer_bounds);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}