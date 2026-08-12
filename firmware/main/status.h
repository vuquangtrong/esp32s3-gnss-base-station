#ifndef STATUS_H
#define STATUS_H

#define STT_VALUE_LENGTH_MAX (64)

typedef enum
{
    // Battery
    STT_BAT_VOLT,
    //
    STT_MAX
} status_type_t;

typedef enum
{
    STT_VALUE_INT,
    STT_VALUE_DOUBLE,
    STT_VALUE_STRING
} status_value_type_t;

typedef union
{
    int i_value;
    double d_value;
    char str_value[STT_VALUE_LENGTH_MAX];
} status_value_u;

typedef struct
{
    const char* name;
    status_value_type_t type;
    status_value_u value;
} status_entry_t;

const char* status_name(status_type_t key);
status_value_type_t status_type(status_type_t key);

void status_set_int(status_type_t key, int value);
void status_set_double(status_type_t key, double value);
void status_set_str(status_type_t key, const char* value);

int status_get_int(status_type_t key);
double status_get_double(status_type_t key);
const char* status_get_str(status_type_t key);

const char* status_get_all(void);

// Macro to automatically select the appropriate status_set function based on the type of the value passed in
#define status_set(key, val)                                                                                                                                              \
    _Generic((+val), int: status_set_int, long: status_set_int, double: status_set_double, float: status_set_double, char*: status_set_str, const char*: status_set_str)( \
        key, val                                                                                                                                                          \
    )

#endif  // STATUS_H
