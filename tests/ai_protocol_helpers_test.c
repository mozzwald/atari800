/*
 * Standalone build-safe tests for AI protocol helpers.
 * Compile manually, for example:
 *   cc -I../src -o ai_protocol_helpers_test ai_protocol_helpers_test.c
 * These tests do not start the emulator.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/ai_protocol.h"

static void test_json_object_validation(void)
{
    assert(AI_JSON_IsValidObject("{\"cmd\":\"ping\"}"));
    assert(!AI_JSON_IsValidObject(""));
    assert(!AI_JSON_IsValidObject("[\"cmd\"]"));
    assert(!AI_JSON_IsValidObject("{\"cmd\":\"ping}"));
}

static void test_string_int_bool_fields(void)
{
    const char *json = "{\"cmd\":\"hello\",\"frames\":10,\"enabled\":true}";
    char value[32];
    int number = 0;
    int boolean = 0;

    assert(AI_JSON_GetString(json, "cmd", value, sizeof(value), 1) == AI_JSON_OK);
    assert(strcmp(value, "hello") == 0);
    assert(AI_JSON_GetString(json, "missing", value, sizeof(value), 1) == AI_JSON_MISSING);
    assert(AI_JSON_GetString(json, "frames", value, sizeof(value), 1) == AI_JSON_BAD_TYPE);

    assert(AI_JSON_GetInt(json, "frames", &number, 0, 1, 1, 100) == AI_JSON_OK);
    assert(number == 10);
    assert(AI_JSON_GetInt(json, "frames", &number, 0, 1, 11, 100) == AI_JSON_BAD_VALUE);
    assert(AI_JSON_GetInt(json, "cmd", &number, 0, 1, 0, 100) == AI_JSON_BAD_TYPE);

    assert(AI_JSON_GetBool(json, "enabled", &boolean, 0, 1) == AI_JSON_OK);
    assert(boolean == 1);
    assert(AI_JSON_GetBool(json, "frames", &boolean, 0, 1) == AI_JSON_BAD_TYPE);
}

static void test_byte_array(void)
{
    const char *json = "{\"data\":[0,1,255]}";
    unsigned char bytes[4];
    int count = 0;

    assert(AI_JSON_GetByteArray(json, "data", bytes, sizeof(bytes), &count, 1) == AI_JSON_OK);
    assert(count == 3);
    assert(bytes[0] == 0);
    assert(bytes[1] == 1);
    assert(bytes[2] == 255);
    assert(AI_JSON_GetByteArray("{\"data\":[256]}", "data", bytes, sizeof(bytes), &count, 1) == AI_JSON_BAD_VALUE);
    assert(AI_JSON_GetByteArray("{\"data\":[0,1,2,3,4]}", "data", bytes, sizeof(bytes), &count, 1) == AI_JSON_NO_SPACE);
}

static void test_json_escaping(void)
{
    char out[128];
    size_t pos = 0;

    assert(AI_JSON_EscapeAppend(out, sizeof(out), &pos, "quote=\" slash=\\ newline=\n"));
    assert(strcmp(out, "\"quote=\\\" slash=\\\\ newline=\\n\"") == 0);
}

int main(void)
{
    test_json_object_validation();
    test_string_int_bool_fields();
    test_byte_array();
    test_json_escaping();
    puts("ai_protocol_helpers_test: ok");
    return 0;
}
