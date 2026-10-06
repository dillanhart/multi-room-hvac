#ifndef CONTROL_H
#define CONTROL_H

#include <string>
#include <vector>
#include "sensor.h"
#include "validation.h"   // sensor_node

enum class hvac_command{
    emergency_heat,
    heat,
    cool,
    fan,
    none,
    no_data,
};

enum class hvac_status{
    em_heating,
    heating,
    cooling,
    circulating,
    idle,
};

hvac_command get_HVAC_command(const std::vector<sensor_node>& sensors, float heat_set, float ac_set);

hvac_status test_switch (hvac_status sys_status, hvac_command command,long long cycle_start, long long now_ms, long long cycle_end);


#endif
