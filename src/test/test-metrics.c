/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <sys/socket.h>

#include "sd-event.h"
#include "sd-json.h"
#include "sd-varlink.h"

#include "fd-util.h"
#include "metrics.h"
#include "string-util.h"
#include "tests.h"
#include "varlink-io.systemd.Metrics.h"
#include "varlink-util.h"

static const uint64_t test_bounds[] = { 10, 100, 1000 };

TEST(histogram_observe) {
        uint64_t counts[ELEMENTSOF(test_bounds) + 1] = {}, sum = 0;

        FOREACH_ELEMENT(v, ((const uint64_t[]) { 0, 5, 10, 11, 1000, 1001, 5000 }))
                metric_histogram_observe(test_bounds, ELEMENTSOF(test_bounds), counts, &sum, *v);

        /* Bounds are inclusive, values above the last bound land in the extra counter */
        ASSERT_EQ(counts[0], 3U);
        ASSERT_EQ(counts[1], 1U);
        ASSERT_EQ(counts[2], 1U);
        ASSERT_EQ(counts[3], 2U);
        ASSERT_EQ(sum, 7027U);
}

typedef struct TestHistogram {
        const char *object;
        uint64_t counts[ELEMENTSOF(test_bounds) + 1];
        uint64_t sum;
        const char *field;
        unsigned n_replies;
} TestHistogram;

static const MetricFamily test_metric_family = {
        .name = "io.systemd.Test.Histogram",
        .description = "Test histogram",
        .type = METRIC_FAMILY_TYPE_HISTOGRAM,
};

static int method_list(sd_varlink *link, sd_json_variant *parameters, sd_varlink_method_flags_t flags, void *userdata) {
        TestHistogram *histograms = ASSERT_PTR(userdata);

        ASSERT_OK(sd_varlink_set_sentinel(link, "io.systemd.Metrics.NoSuchMetric"));

        for (TestHistogram *h = histograms; h->object; h++) {
                _cleanup_(sd_json_variant_unrefp) sd_json_variant *fields = NULL;

                if (h->field)
                        ASSERT_OK(sd_json_buildo(&fields, SD_JSON_BUILD_PAIR_STRING("kind", h->field)));

                ASSERT_OK(metric_build_send_histogram(
                                          &test_metric_family,
                                          link,
                                          h->object,
                                          test_bounds,
                                          ELEMENTSOF(test_bounds),
                                          h->counts,
                                          h->sum,
                                          fields));
        }

        return 0;
}

static int reply_list(sd_varlink *link, sd_json_variant *parameters, const char *error_id, sd_varlink_reply_flags_t flags, void *userdata) {
        TestHistogram *histograms = ASSERT_PTR(userdata), *h;

        ASSERT_NULL(error_id);
        ASSERT_STREQ(sd_json_variant_string(sd_json_variant_by_key(parameters, "name")), test_metric_family.name);

        const char *object = sd_json_variant_string(sd_json_variant_by_key(parameters, "object"));
        ASSERT_NOT_NULL(object);
        for (h = histograms; h->object; h++)
                if (streq(h->object, object))
                        break;
        ASSERT_NOT_NULL(h->object);
        h->n_replies++;

        sd_json_variant *fields = sd_json_variant_by_key(parameters, "fields");
        if (h->field)
                ASSERT_STREQ(sd_json_variant_string(sd_json_variant_by_key(fields, "kind")), h->field);
        else
                ASSERT_NULL(fields);

        sd_json_variant *value = sd_json_variant_by_key(parameters, "value");
        ASSERT_TRUE(sd_json_variant_is_object(value));
        ASSERT_EQ(sd_json_variant_elements(value), 6U);
        ASSERT_EQ(sd_json_variant_unsigned(sd_json_variant_by_key(value, "sum")), h->sum);

        /* Bucket counts are cumulative, and the total count includes the values above the last bound */
        sd_json_variant *buckets = sd_json_variant_by_key(value, "buckets");
        ASSERT_TRUE(sd_json_variant_is_array(buckets));
        ASSERT_EQ(sd_json_variant_elements(buckets), ELEMENTSOF(test_bounds));

        uint64_t total = 0;
        for (size_t i = 0; i < ELEMENTSOF(test_bounds); i++) {
                sd_json_variant *bucket = sd_json_variant_by_index(buckets, i);

                total += h->counts[i];
                ASSERT_EQ(sd_json_variant_elements(bucket), 4U);
                ASSERT_EQ(sd_json_variant_unsigned(sd_json_variant_by_key(bucket, "upperBound")), test_bounds[i]);
                ASSERT_EQ(sd_json_variant_unsigned(sd_json_variant_by_key(bucket, "count")), total);
        }

        total += h->counts[ELEMENTSOF(test_bounds)];
        ASSERT_EQ(sd_json_variant_unsigned(sd_json_variant_by_key(value, "count")), total);

        if (!FLAGS_SET(flags, SD_VARLINK_REPLY_CONTINUES))
                ASSERT_OK(sd_event_exit(sd_varlink_get_event(link), /* code= */ 0));

        return 0;
}

TEST(histogram_send) {
        _cleanup_(sd_event_unrefp) sd_event *event = NULL;
        _cleanup_(sd_varlink_server_unrefp) sd_varlink_server *server = NULL;
        _cleanup_(sd_varlink_unrefp) sd_varlink *client = NULL;
        _cleanup_close_pair_ int pair[2] = EBADF_PAIR;

        TestHistogram histograms[] = {
                { .object = "empty" },
                { .object = "first-bucket", .counts = { 3 }, .sum = 15, .field = "a" },
                { .object = "all-buckets", .counts = { 1, 2, 3, 4 }, .sum = 5432, .field = "b" },
                { .object = "above-last-bound", .counts = { 0, 0, 0, 7 }, .sum = UINT64_MAX },
                {},
        };

        ASSERT_OK(sd_event_default(&event));
        ASSERT_OK(varlink_server_new(&server, SD_VARLINK_SERVER_INHERIT_USERDATA, histograms));
        ASSERT_OK(sd_varlink_server_add_interface(server, &vl_interface_io_systemd_Metrics));
        ASSERT_OK(sd_varlink_server_bind_method(server, "io.systemd.Metrics.List", method_list));
        ASSERT_OK(sd_varlink_server_attach_event(server, event, /* priority= */ 0));
        ASSERT_OK_ERRNO(socketpair(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0, pair));
        ASSERT_OK(sd_varlink_server_add_connection(server, pair[0], /* ret= */ NULL));
        TAKE_FD(pair[0]);
        ASSERT_OK(sd_varlink_connect_fd(&client, pair[1]));
        TAKE_FD(pair[1]);
        sd_varlink_set_userdata(client, histograms);
        ASSERT_OK(sd_varlink_bind_reply(client, reply_list));
        ASSERT_OK(sd_varlink_attach_event(client, event, /* priority= */ 0));
        ASSERT_OK(sd_varlink_observe(client, "io.systemd.Metrics.List", /* parameters= */ NULL));
        ASSERT_OK(sd_event_loop(event));

        for (TestHistogram *h = histograms; h->object; h++)
                ASSERT_EQ(h->n_replies, 1U);
}

DEFINE_TEST_MAIN(LOG_INFO);
