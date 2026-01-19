#include "json_util.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
  char *escaped = json_escape("line1\nline2\"\\");
  assert(escaped != NULL);
  assert(strcmp(escaped, "line1\\nline2\\\"\\\\") == 0);

  char *unescaped = json_unescape(escaped);
  assert(unescaped != NULL);
  assert(strcmp(unescaped, "line1\nline2\"\\") == 0);

  free(escaped);
  free(unescaped);

  const char *json = "{\"type\":\"offer\",\"sdp\":\"v=0\\r\\n\"}";
  char *type = json_get_string(json, "type");
  char *sdp = json_get_string(json, "sdp");
  assert(type != NULL);
  assert(sdp != NULL);
  assert(strcmp(type, "offer") == 0);
  assert(strcmp(sdp, "v=0\r\n") == 0);

  free(type);
  free(sdp);

  const char *json2 = "{\"sdpMLineIndex\":2}";
  assert(json_get_int(json2, "sdpMLineIndex", 0) == 2);
  assert(json_get_int(json2, "missing", 7) == 7);

  printf("json_util tests passed\n");
  return 0;
}
