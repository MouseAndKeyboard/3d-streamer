#include "json_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *json_escape(const char *input) {
  size_t len = 0;
  for (const char *p = input; *p; ++p) {
    switch (*p) {
      case '"':
      case '\\':
      case '\n':
      case '\r':
      case '\t':
        len += 2;
        break;
      default:
        len += 1;
        break;
    }
  }

  char *out = malloc(len + 1);
  size_t idx = 0;
  for (const char *p = input; *p; ++p) {
    switch (*p) {
      case '"':
        out[idx++] = '\\';
        out[idx++] = '"';
        break;
      case '\\':
        out[idx++] = '\\';
        out[idx++] = '\\';
        break;
      case '\n':
        out[idx++] = '\\';
        out[idx++] = 'n';
        break;
      case '\r':
        out[idx++] = '\\';
        out[idx++] = 'r';
        break;
      case '\t':
        out[idx++] = '\\';
        out[idx++] = 't';
        break;
      default:
        out[idx++] = *p;
        break;
    }
  }
  out[idx] = '\0';
  return out;
}

char *json_unescape(const char *input) {
  size_t len = strlen(input);
  char *out = malloc(len + 1);
  size_t idx = 0;
  for (size_t i = 0; i < len; ++i) {
    if (input[i] == '\\' && i + 1 < len) {
      ++i;
      switch (input[i]) {
        case 'n':
          out[idx++] = '\n';
          break;
        case 'r':
          out[idx++] = '\r';
          break;
        case 't':
          out[idx++] = '\t';
          break;
        case '"':
          out[idx++] = '"';
          break;
        case '\\':
          out[idx++] = '\\';
          break;
        default:
          out[idx++] = input[i];
          break;
      }
    } else {
      out[idx++] = input[i];
    }
  }
  out[idx] = '\0';
  return out;
}

char *json_get_string(const char *json, const char *key) {
  char needle[64];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char *pos = strstr(json, needle);
  if (!pos) {
    return NULL;
  }
  pos = strchr(pos + strlen(needle), ':');
  if (!pos) {
    return NULL;
  }
  pos++;
  while (*pos == ' ' || *pos == '\t') {
    pos++;
  }
  if (*pos != '"') {
    return NULL;
  }
  pos++;
  const char *start = pos;
  int escape = 0;
  while (*pos) {
    if (!escape && *pos == '"') {
      break;
    }
    if (!escape && *pos == '\\') {
      escape = 1;
    } else {
      escape = 0;
    }
    pos++;
  }
  size_t raw_len = (size_t)(pos - start);
  char *raw = malloc(raw_len + 1);
  memcpy(raw, start, raw_len);
  raw[raw_len] = '\0';
  char *out = json_unescape(raw);
  free(raw);
  return out;
}

int json_get_int(const char *json, const char *key, int def_value) {
  char needle[64];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char *pos = strstr(json, needle);
  if (!pos) {
    return def_value;
  }
  pos = strchr(pos + strlen(needle), ':');
  if (!pos) {
    return def_value;
  }
  pos++;
  while (*pos == ' ' || *pos == '\t') {
    pos++;
  }
  return atoi(pos);
}
