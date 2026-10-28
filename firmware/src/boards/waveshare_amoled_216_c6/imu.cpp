#include "../../hal/imu_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <SensorQMI8658.hpp>

// QMI8658 is populated on the 2.16 carrier PCB but rotation is disabled
// on the C6 build (BOARD_HAS_ROTATION=0). We still initialize the device
// so the shared I2C bus stays healthy and so a future build can flip
// rotation on without changing this file. Always reports quadrant 0.

static SensorQMI8658 imu;
static bool imu_ok = false;

void imu_hal_init(void) {
    if (!imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
        Serial.println("QMI8658 init failed");
        return;
    }
    Serial.println("QMI8658 init OK (rotation disabled on this board)");
    // Accelerometer for the motion features (shake / pick-up / face-down).
    imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_62_5Hz,
                            SensorQMI8658::LPF_MODE_0);
    imu.enableAccelerometer();
    imu_ok = true;
}

void imu_hal_tick(void) {
    // No-op — rotation is disabled.
}

uint8_t imu_hal_rotation_quadrant(void) { return 0; }

bool imu_hal_read_accel(float* x, float* y, float* z) {
    if (!imu_ok) return false;
    return imu.getAccelerometer(*x, *y, *z);
}
