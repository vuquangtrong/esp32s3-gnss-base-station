#ifndef GNSS_H
#define GNSS_H

#define UBX_COMMAND_LEN_MAX 256

void gnss_set_mode_rover(void);
void gnss_set_mode_base(void);
void gnss_set_mode_ppp(void);
void gnss_base_set_fixed(
    double latitude,   // deg, 9 decimal places
    double longitude,  // deg, 9 decimal places
    double height      // m, 4 decimal places
);
void gnss_base_set_survey_in(
    int duration,  // seconds
    int accuracy   // mm
);

#endif  // GNSS_H
