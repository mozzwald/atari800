/*
 * ai_protocol.h - shared helpers for the Atari800 AI socket protocol
 *
 * Copyright (c) 2026 - AI Interface Extension
 * Licensed under GPL-2.0-or-later
 */

#ifndef AI_PROTOCOL_H_
#define AI_PROTOCOL_H_

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AI_PROTOCOL_VERSION 1
#define AI_PEEK_MAX_LEN 256
#define AI_DEFAULT_MAX_COMMAND_BYTES 65536
#define AI_DEFAULT_MAX_RESPONSE_BYTES 1048576

#define AI_JSON_OK 0
#define AI_JSON_MISSING 1
#define AI_JSON_BAD_TYPE 2
#define AI_JSON_BAD_VALUE 3
#define AI_JSON_NO_SPACE 4

static int AI_JSON_IsValidObject(const char *json)
{
    const char *p;
    int depth = 0;
    int in_string = 0;
    int escape = 0;

    if (json == NULL)
        return 0;
    p = json;
    while (isspace((unsigned char)*p))
        p++;
    if (*p != '{')
        return 0;

    for (; *p != '\0'; p++) {
        char c = *p;
        if (in_string) {
            if (escape) {
                escape = 0;
            }
            else if (c == '\\') {
                escape = 1;
            }
            else if (c == '"') {
                in_string = 0;
            }
            continue;
        }
        if (c == '"') {
            in_string = 1;
        }
        else if (c == '{' || c == '[') {
            depth++;
        }
        else if (c == '}' || c == ']') {
            depth--;
            if (depth < 0)
                return 0;
        }
    }
    return !in_string && depth == 0;
}

static const char *AI_JSON_SkipSpace(const char *p)
{
    while (p != NULL && isspace((unsigned char)*p))
        p++;
    return p;
}

static const char *AI_JSON_SkipString(const char *p)
{
    int escape = 0;
    if (p == NULL || *p != '"')
        return NULL;
    for (p++; *p != '\0'; p++) {
        if (escape) {
            escape = 0;
        }
        else if (*p == '\\') {
            escape = 1;
        }
        else if (*p == '"') {
            return p + 1;
        }
    }
    return NULL;
}

static int AI_JSON_StringEquals(const char *p, const char *key)
{
    const char *k = key;
    int escape = 0;

    if (p == NULL || key == NULL || *p != '"')
        return 0;
    for (p++; *p != '\0'; p++) {
        if (escape) {
            if (*k == '\0' || *k++ != *p)
                return 0;
            escape = 0;
        }
        else if (*p == '\\') {
            escape = 1;
        }
        else if (*p == '"') {
            return *k == '\0';
        }
        else if (*k == '\0' || *k++ != *p) {
            return 0;
        }
    }
    return 0;
}

static const char *AI_JSON_FindValue(const char *json, const char *key)
{
    const char *p;

    if (json == NULL || key == NULL)
        return NULL;
    p = AI_JSON_SkipSpace(json);
    if (p == NULL || *p != '{')
        return NULL;
    p++;

    while (*p != '\0') {
        p = AI_JSON_SkipSpace(p);
        if (*p == '}')
            return NULL;
        if (*p != '"')
            return NULL;
        if (AI_JSON_StringEquals(p, key)) {
            p = AI_JSON_SkipString(p);
            p = AI_JSON_SkipSpace(p);
            if (p == NULL || *p != ':')
                return NULL;
            return AI_JSON_SkipSpace(p + 1);
        }
        p = AI_JSON_SkipString(p);
        p = AI_JSON_SkipSpace(p);
        if (p == NULL || *p != ':')
            return NULL;
        p = AI_JSON_SkipSpace(p + 1);
        if (p == NULL)
            return NULL;
        if (*p == '"') {
            p = AI_JSON_SkipString(p);
        }
        else {
            int depth = 0;
            int in_string = 0;
            int escape = 0;
            for (; *p != '\0'; p++) {
                if (in_string) {
                    if (escape)
                        escape = 0;
                    else if (*p == '\\')
                        escape = 1;
                    else if (*p == '"')
                        in_string = 0;
                    continue;
                }
                if (*p == '"')
                    in_string = 1;
                else if (*p == '[' || *p == '{')
                    depth++;
                else if (*p == ']' || *p == '}') {
                    if (depth == 0)
                        break;
                    depth--;
                }
                else if (*p == ',' && depth == 0)
                    break;
            }
        }
        p = AI_JSON_SkipSpace(p);
        if (*p == ',')
            p++;
    }
    return NULL;
}

static int AI_JSON_UnescapeString(const char *p, char *buf, int bufsize)
{
    int out = 0;

    if (p == NULL || buf == NULL || bufsize <= 0 || *p != '"')
        return AI_JSON_BAD_TYPE;
    p++;
    while (*p != '\0' && *p != '"') {
        char c = *p++;
        if (c == '\\') {
            c = *p++;
            switch (c) {
            case '"': case '\\': case '/': break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u':
                if (!isxdigit((unsigned char)p[0]) || !isxdigit((unsigned char)p[1]) ||
                    !isxdigit((unsigned char)p[2]) || !isxdigit((unsigned char)p[3]))
                    return AI_JSON_BAD_VALUE;
                p += 4;
                c = '?';
                break;
            default:
                return AI_JSON_BAD_VALUE;
            }
        }
        if (out >= bufsize - 1)
            return AI_JSON_NO_SPACE;
        buf[out++] = c;
    }
    if (*p != '"')
        return AI_JSON_BAD_VALUE;
    buf[out] = '\0';
    return AI_JSON_OK;
}

static int AI_JSON_GetString(const char *json, const char *key, char *buf, int bufsize, int required)
{
    const char *p = AI_JSON_FindValue(json, key);
    if (p == NULL) {
        if (buf != NULL && bufsize > 0)
            buf[0] = '\0';
        return required ? AI_JSON_MISSING : AI_JSON_OK;
    }
    if (*p != '"')
        return AI_JSON_BAD_TYPE;
    return AI_JSON_UnescapeString(p, buf, bufsize);
}

static int AI_JSON_GetInt(const char *json, const char *key, int *value, int def, int required, int min_value, int max_value)
{
    const char *p = AI_JSON_FindValue(json, key);
    char *endptr;
    long v;

    if (p == NULL) {
        if (value != NULL)
            *value = def;
        return required ? AI_JSON_MISSING : AI_JSON_OK;
    }
    if (*p == '"' || *p == '[' || *p == '{')
        return AI_JSON_BAD_TYPE;
    errno = 0;
    v = strtol(p, &endptr, 0);
    if (p == endptr || errno != 0)
        return AI_JSON_BAD_TYPE;
    endptr = (char *)AI_JSON_SkipSpace(endptr);
    if (*endptr != '\0' && *endptr != ',' && *endptr != '}' && *endptr != ']')
        return AI_JSON_BAD_TYPE;
    if (v < min_value || v > max_value)
        return AI_JSON_BAD_VALUE;
    if (value != NULL)
        *value = (int)v;
    return AI_JSON_OK;
}

static int AI_JSON_GetBool(const char *json, const char *key, int *value, int def, int required)
{
    const char *p = AI_JSON_FindValue(json, key);

    if (p == NULL) {
        if (value != NULL)
            *value = def;
        return required ? AI_JSON_MISSING : AI_JSON_OK;
    }
    if (strncmp(p, "true", 4) == 0) {
        if (value != NULL)
            *value = 1;
        return AI_JSON_OK;
    }
    if (strncmp(p, "false", 5) == 0) {
        if (value != NULL)
            *value = 0;
        return AI_JSON_OK;
    }
    return AI_JSON_BAD_TYPE;
}

static int AI_JSON_GetByteArray(const char *json, const char *key, unsigned char *buf, int bufsize, int *count, int required)
{
    const char *p = AI_JSON_FindValue(json, key);
    int n = 0;

    if (count != NULL)
        *count = 0;
    if (p == NULL)
        return required ? AI_JSON_MISSING : AI_JSON_OK;
    if (*p != '[')
        return AI_JSON_BAD_TYPE;
    p++;
    for (;;) {
        char *endptr;
        long v;
        p = AI_JSON_SkipSpace(p);
        if (*p == ']') {
            if (count != NULL)
                *count = n;
            return AI_JSON_OK;
        }
        if (n >= bufsize)
            return AI_JSON_NO_SPACE;
        errno = 0;
        v = strtol(p, &endptr, 0);
        if (p == endptr || errno != 0)
            return AI_JSON_BAD_TYPE;
        if (v < 0 || v > 255)
            return AI_JSON_BAD_VALUE;
        buf[n++] = (unsigned char)v;
        p = AI_JSON_SkipSpace(endptr);
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p == ']') {
            if (count != NULL)
                *count = n;
            return AI_JSON_OK;
        }
        return AI_JSON_BAD_TYPE;
    }
}

static int AI_JSON_EscapeAppend(char *dst, size_t dst_size, size_t *pos, const char *src)
{
    const unsigned char *p = (const unsigned char *)src;
    if (dst == NULL || pos == NULL || *pos >= dst_size)
        return 0;
    if (*pos + 1 >= dst_size)
        return 0;
    dst[(*pos)++] = '"';
    while (p != NULL && *p != '\0') {
        char esc[8];
        const char *out = esc;
        size_t len;
        switch (*p) {
        case '"': out = "\\\""; break;
        case '\\': out = "\\\\"; break;
        case '\b': out = "\\b"; break;
        case '\f': out = "\\f"; break;
        case '\n': out = "\\n"; break;
        case '\r': out = "\\r"; break;
        case '\t': out = "\\t"; break;
        default:
            if (*p < 0x20) {
                snprintf(esc, sizeof(esc), "\\u%04x", *p);
            }
            else {
                esc[0] = (char)*p;
                esc[1] = '\0';
            }
            break;
        }
        len = strlen(out);
        if (*pos + len + 1 >= dst_size)
            return 0;
        memcpy(dst + *pos, out, len);
        *pos += len;
        p++;
    }
    if (*pos + 1 >= dst_size)
        return 0;
    dst[(*pos)++] = '"';
    dst[*pos] = '\0';
    return 1;
}

static const char *AI_JSON_ErrorCode(int rc, int required)
{
    switch (rc) {
    case AI_JSON_MISSING:
        return required ? "MISSING_FIELD" : "BAD_ARGUMENT";
    case AI_JSON_BAD_TYPE:
        return "BAD_ARGUMENT";
    case AI_JSON_BAD_VALUE:
        return "BAD_ARGUMENT";
    case AI_JSON_NO_SPACE:
        return "BAD_ARGUMENT";
    default:
        return "BAD_ARGUMENT";
    }
}

#endif /* AI_PROTOCOL_H_ */
