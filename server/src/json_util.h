#ifndef JSON_UTIL_H
#define JSON_UTIL_H

char *json_escape(const char *input);
char *json_unescape(const char *input);
char *json_get_string(const char *json, const char *key);
int json_get_int(const char *json, const char *key, int def_value);

#endif
