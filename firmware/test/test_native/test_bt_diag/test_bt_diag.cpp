// Native tests for BtDiag: Bluetooth status codes -> words and categories.

#include <unity.h>

#include <string.h>

#include "bt_diag.h"

void setUp() {}
void tearDown() {}

namespace {
constexpr int hci(int code) { return 0x100 + code; }
}

static void test_names_for_the_codes_we_expect() {
    TEST_ASSERT_EQUAL_STRING("ok", btStatusName(0));
    TEST_ASSERT_EQUAL_STRING("page timeout", btStatusName(hci(0x04)));
    TEST_ASSERT_EQUAL_STRING("authentication failure", btStatusName(hci(0x05)));
    TEST_ASSERT_EQUAL_STRING("pin or key missing", btStatusName(hci(0x06)));
    TEST_ASSERT_EQUAL_STRING("pairing not allowed", btStatusName(hci(0x18)));
    TEST_ASSERT_EQUAL_STRING("rejected: security", btStatusName(hci(0x0E)));
    TEST_ASSERT_EQUAL_STRING("remote user ended", btStatusName(hci(0x13)));
    TEST_ASSERT_NULL(btStatusName(hci(0x7E)));
}

static void test_classification() {
    TEST_ASSERT_EQUAL(BtFailure::None, classifyBtStatus(0));
    TEST_ASSERT_EQUAL(BtFailure::None, classifyBtStatus(hci(0)));
    TEST_ASSERT_EQUAL(BtFailure::NoAnswer, classifyBtStatus(hci(0x04)));
    TEST_ASSERT_EQUAL(BtFailure::NoAnswer, classifyBtStatus(hci(0x08)));
    TEST_ASSERT_EQUAL(BtFailure::StaleKey, classifyBtStatus(hci(0x06)));
    TEST_ASSERT_EQUAL(BtFailure::Refused, classifyBtStatus(hci(0x18)));
    TEST_ASSERT_EQUAL(BtFailure::Refused, classifyBtStatus(hci(0x0D)));
    TEST_ASSERT_EQUAL(BtFailure::Refused, classifyBtStatus(hci(0x0E)));
    TEST_ASSERT_EQUAL(BtFailure::Refused, classifyBtStatus(hci(0x0F)));
    TEST_ASSERT_EQUAL(BtFailure::Refused, classifyBtStatus(hci(0x05)));
    TEST_ASSERT_EQUAL(BtFailure::Refused, classifyBtStatus(9));   // stack: auth failure
    TEST_ASSERT_EQUAL(BtFailure::Refused, classifyBtStatus(11));  // stack: auth rejected
    TEST_ASSERT_EQUAL(BtFailure::Other, classifyBtStatus(hci(0x0C)));
}

static void test_failure_names() {
    TEST_ASSERT_EQUAL_STRING("no answer", btFailureName(BtFailure::NoAnswer));
    TEST_ASSERT_EQUAL_STRING("refused", btFailureName(BtFailure::Refused));
    TEST_ASSERT_EQUAL_STRING("stale pairing key", btFailureName(BtFailure::StaleKey));
}

static void test_legacy_pins_alternate_starting_with_edifier_default() {
    TEST_ASSERT_EQUAL_STRING("0000", legacyPinForAttempt(0));
    TEST_ASSERT_EQUAL_STRING("1234", legacyPinForAttempt(1));
    TEST_ASSERT_EQUAL_STRING("0000", legacyPinForAttempt(2));
    TEST_ASSERT_EQUAL_STRING("0000", legacyPinForAttempt(-1));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_names_for_the_codes_we_expect);
    RUN_TEST(test_classification);
    RUN_TEST(test_failure_names);
    RUN_TEST(test_legacy_pins_alternate_starting_with_edifier_default);
    return UNITY_END();
}
