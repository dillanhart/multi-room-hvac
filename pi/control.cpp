#include <stdio.h>
#include "sensor.h"
#include <string>
#include <vector>
#include <stdbool.h>
#include <chrono>
#include <iostream>



std::string get_HVAC_command(sensor_result main_data, sensor_result office_data, sensor_result bed_data, float heat_set, float ac_set, float FAN_TRIGGER, float FAN_CLEAR){

    int main_weight, office_weight, bed_weight;

    bool day_weights = hour_of_day();
    // get the variable data from the sensor struct and assign weights
    auto [main_temp, main_hum, main_co2, main_valid] = main_data;
    if (test_valid_temp(main_temp, main_valid)) {
        main_weight = day_weights ? 10 : 8;
    } else {
        main_weight = 0;
    }


    auto [office_temp, office_hum, office_co2, office_valid] = office_data;
    if (test_valid_temp(office_temp, office_valid)) {
        office_weight = day_weights ? 6 : 4;
    } else {
        office_weight = 0;
    }

    auto [bed_temp, bed_hum, bed_co2, bed_valid] = bed_data;
    if (test_valid_temp(bed_temp, bed_valid)) {
        bed_weight = day_weights ? 6 : 4;
    } else {
        bed_weight = 0;
    }

    float temps[3] = {main_temp, office_temp, bed_temp};
    int weights[3] = {main_weight, office_weight, bed_weight};

    float weighted_temp = get_weighted_temp(temps, weights);

    if (weighted_temp > ac_set) return "cool";
    if (weighted_temp < heat_set) return "heat";



}

bool test_valid_temp(float temp, int valid){
    return (temp > 50.0 && temp < 110 && valid == 1);
}


float get_weighted_temp(float temps[], int weights[]){
    float weighted_temp;
    for (int i = 0; i < 3; i++){
        weighted_temp += temps[i] * weights[i];
    }
    return weighted_temp;
}

bool hour_of_day (){
    std::time_t now = std::time(nullptr);
    std::tm* local_time = std::localtime(&now);
    return local_time->tm_hour > 8;
}
