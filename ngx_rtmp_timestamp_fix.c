struct ts
{
    u_char *name;
    uint32_t *timestamp;
    struct ts *next;
};

struct ts *root_ts;

uint32_t *get_offset (u_char *name, uint32_t *current_ts) {
  if (root_ts == NULL) {
    root_ts = malloc(sizeof(struct ts));
    root_ts->timestamp = malloc(sizeof(uint32_t));
    root_ts->name = name;
    *root_ts->timestamp = *current_ts;
    root_ts->next = NULL;
    return NULL;
  }

  struct ts *cur = root_ts;
  struct ts *last = root_ts;
  while (cur != NULL) {
    if (strcmp((char *) name, (char *) cur->name) == 0) {
      break;
    }
    last = cur;
    cur = cur->next;  
  }

  // not found
  if (cur == NULL) {    
    struct ts *next = malloc(sizeof(struct ts));
    next->name = name;
    next->timestamp = malloc(sizeof(uint32_t));
    *next->timestamp = *current_ts;
    next->next = NULL;
    last->next = next;
    return NULL;
  } else {
    // found
    if (*current_ts < *cur->timestamp) {
      return cur->timestamp;
    }
    *cur->timestamp = *current_ts;
    return NULL;
  }
}



