/*
 * Test SASL Parser
 */

#include <c-macro.h>
#include <stdlib.h>
#include "sasl.h"

static void test_setup(void) {
        SASL sasl;

        sasl_init(&sasl, 1, "123456789abcdef");
        sasl_deinit(&sasl);
}

static void assert_dispatch(SASL *sasl, char *in, char *out, int ret) {
        const char *reply;
        size_t n_reply;
        int r;

        r = sasl_dispatch(sasl, in, &reply, &n_reply);
        assert(r == ret);
        if (r == 0) {
                assert(n_reply == strlen(out));
                assert(strcmp(reply, out) == 0);
        }
}

/* discover the available mechanisms */
static void test_discover(void) {
        SASL sasl;

        sasl_init(&sasl, 1, "0123456789abcdef");

        assert_dispatch(&sasl, "AUTH", "REJECTED EXTERNAL\r\n", 0);

        sasl_deinit(&sasl);
}

/* test external */
static void test_external(void) {
        SASL sasl;

        sasl_init(&sasl, 1, "0123456789abcdef");

        assert_dispatch(&sasl, "AUTH EXTERNAL 31", "OK 30313233343536373839616263646566\r\n", 0);
        assert_dispatch(&sasl, "BEGIN", "", 1);

        sasl_deinit(&sasl);
}

/* verify that authentiacitng with the wrong uid fails, but retrying succeeds */
static void test_external_invalid(void) {
        SASL sasl;

        sasl_init(&sasl, 1, "0123456789abcdef");

        assert_dispatch(&sasl, "AUTH EXTERNAL 30", "REJECTED EXTERNAL\r\n", 0);
        assert_dispatch(&sasl, "AUTH EXTERNAL 31", "OK 30313233343536373839616263646566\r\n", 0);
        assert_dispatch(&sasl, "BEGIN", "", 1);

        sasl_deinit(&sasl);
}

/* do not supply a uid, but allow the system to use the one it has */
static void test_external_no_data(void) {
        SASL sasl;

        sasl_init(&sasl, 1, "0123456789abcdef");

        assert_dispatch(&sasl, "AUTH EXTERNAL", "DATA\r\n", 0);
        assert_dispatch(&sasl, "DATA", "OK 30313233343536373839616263646566\r\n", 0);
        assert_dispatch(&sasl, "BEGIN", "", 1);

        sasl_deinit(&sasl);
}

/* external and negotiate fds, this is the common case */
static void test_external_fds(void) {
        SASL sasl;

        sasl_init(&sasl, 1, "0123456789abcdef");

        assert_dispatch(&sasl, "AUTH EXTERNAL 31", "OK 30313233343536373839616263646566\r\n", 0);
        assert_dispatch(&sasl, "NEGOTIATE_UNIX_FD", "AGREE_UNIX_FD\r\n", 0);
        assert_dispatch(&sasl, "BEGIN", "", 1);

        sasl_deinit(&sasl);
}

int main(int argc, char **argv) {
        test_setup();
        test_discover();
        test_external();
        test_external_invalid();
        test_external_no_data();
        test_external_fds();
        return 0;
}
