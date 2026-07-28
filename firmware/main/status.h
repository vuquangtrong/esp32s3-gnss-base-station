#ifndef STATUS_H
#define STATUS_H

#define STT_VALUE_LENGTH_MAX 64

typedef enum
{
    STT_STA_CONNECTED,
    STT_MAX
} status_type_t;

typedef struct
{
    const char* name;
    char value[STT_VALUE_LENGTH_MAX];
} status_entry_t;

const char* status_name(status_type_t key);
const char* status_get(status_type_t key);
void status_set(status_type_t key, const char* value);

#endif  // STATUS_H
