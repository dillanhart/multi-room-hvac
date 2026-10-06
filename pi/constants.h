// make sure sensor is reading correctly and prevent short cycling HVAC
constexpr int READ_INTERVAL = MIN;
constexpr int MAX_SENSOR_FAILS = 5;

// avoid short cycling and constant running
constexpr int MIN_ON_TIME = 5 * MIN;
constexpr int MIN_OFF_TIME = 10 * MIN;
constexpr int MAX_RUN_TIME = 120 * MIN;

// make sure heat and sensor are working together
constexpr int PROGRESS_WINDOW = 20 * MIN;
constexpr float PROGRESS_MIN_DELTA = 1; // temp must change by at least 1 degree within 20 mins

// make sure the AC and Heat values are far enough apart
constexpr float HYST = 1;
constexpr float MIN_HEAT = 60;
constexpr float MIN_DEAD_BAND = 3;

// temp delta for fan to turn on and turn off
constexpr float FAN_TRIGGER = 2;
constexpr float FAN_CLEAR = 1;