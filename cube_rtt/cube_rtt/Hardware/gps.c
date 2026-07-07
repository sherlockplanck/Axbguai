#include "gps.h"
#include <string.h>
#include <stdlib.h>

static double Convert_to_degrees(char *data)
{
    double temp_data = atof(data);
    int degree = (int)(temp_data / 100);
    double f_degree = (temp_data / 100.0 - degree) * 100 / 60.0;
    double result = degree + f_degree;
    return result;
}

static int nmea_is_type(const char *sentence, const char *type)
{
    return (sentence != NULL &&
            sentence[0] == '$' &&
            sentence[3] == type[0] &&
            sentence[4] == type[1] &&
            sentence[5] == type[2]);
}

static char *find_nmea_type(char *buffer, const char *type)
{
    char *sentence = buffer;

    while ((sentence = strchr(sentence, '$')) != NULL)
    {
        if (nmea_is_type(sentence, type))
        {
            return sentence;
        }
        sentence++;
    }

    return NULL;
}

static int copy_nmea_field(const char *sentence, int field_index, char *out, int out_size)
{
    const char *field_start = sentence;
    const char *field_end;
    int i;
    int len;

    if (sentence == NULL || out == NULL || out_size <= 0)
    {
        return 0;
    }

    out[0] = '\0';
    for (i = 0; i < field_index; i++)
    {
        field_start = strchr(field_start, ',');
        if (field_start == NULL)
        {
            return 0;
        }
        field_start++;
    }

    field_end = field_start;
    while (*field_end != '\0' && *field_end != ',' &&
           *field_end != '*' && *field_end != '\r' && *field_end != '\n')
    {
        field_end++;
    }

    len = (int)(field_end - field_start);
    if (len <= 0 || len >= out_size)
    {
        return 0;
    }

    memcpy(out, field_start, len);
    out[len] = '\0';
    return 1;
}

static void fill_position(GPS_Info_t *out_gps_data,
                          char *lat_str,
                          char ns,
                          char *lon_str,
                          char ew)
{
    double lat = Convert_to_degrees(lat_str);
    double lon = Convert_to_degrees(lon_str);

    if (ns == 'S')
    {
        lat = -lat;
    }
    if (ew == 'W')
    {
        lon = -lon;
    }

    out_gps_data->latitude = lat;
    out_gps_data->longitude = lon;
    out_gps_data->fix_status = 1;
}

static void parse_gga(char *sentence, GPS_Info_t *out_gps_data)
{
    char lat_str[20] = {0};
    char lon_str[20] = {0};
    char ns_str[2] = {0};
    char ew_str[2] = {0};
    char fix_quality[4] = {0};

    out_gps_data->sentence_parsed = 1;

    if (!copy_nmea_field(sentence, 6, fix_quality, sizeof(fix_quality)) ||
        fix_quality[0] <= '0')
    {
        out_gps_data->fix_status = 0;
        return;
    }

    if (copy_nmea_field(sentence, 2, lat_str, sizeof(lat_str)) &&
        copy_nmea_field(sentence, 3, ns_str, sizeof(ns_str)) &&
        copy_nmea_field(sentence, 4, lon_str, sizeof(lon_str)) &&
        copy_nmea_field(sentence, 5, ew_str, sizeof(ew_str)))
    {
        fill_position(out_gps_data, lat_str, ns_str[0], lon_str, ew_str[0]);
    }
    else
    {
        out_gps_data->fix_status = 0;
    }
}

static void parse_rmc(char *sentence, GPS_Info_t *out_gps_data)
{
    char status[2] = {0};
    char lat_str[20] = {0};
    char lon_str[20] = {0};
    char ns_str[2] = {0};
    char ew_str[2] = {0};

    out_gps_data->sentence_parsed = 1;

    if (!copy_nmea_field(sentence, 2, status, sizeof(status)) ||
        status[0] != 'A')
    {
        out_gps_data->fix_status = 0;
        return;
    }

    if (copy_nmea_field(sentence, 3, lat_str, sizeof(lat_str)) &&
        copy_nmea_field(sentence, 4, ns_str, sizeof(ns_str)) &&
        copy_nmea_field(sentence, 5, lon_str, sizeof(lon_str)) &&
        copy_nmea_field(sentence, 6, ew_str, sizeof(ew_str)))
    {
        fill_position(out_gps_data, lat_str, ns_str[0], lon_str, ew_str[0]);
    }
    else
    {
        out_gps_data->fix_status = 0;
    }
}

void GPS_ParseData(uint8_t *nmea_buffer, GPS_Info_t *out_gps_data)
{
    char *sentence;

    if (nmea_buffer == NULL || out_gps_data == NULL)
    {
        return;
    }

    out_gps_data->fix_status = 0;
    out_gps_data->sentence_parsed = 0;

    sentence = find_nmea_type((char *)nmea_buffer, "GGA");
    if (sentence != NULL)
    {
        parse_gga(sentence, out_gps_data);
        return;
    }

    sentence = find_nmea_type((char *)nmea_buffer, "RMC");
    if (sentence != NULL)
    {
        parse_rmc(sentence, out_gps_data);
    }
}
