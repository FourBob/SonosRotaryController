#include "touch.h"

static bool s_touch_active = false;    // true while finger is on screen

int read_touch(int *x, int *y)
{
    uint8_t data_raw[7] = {0};
    if (i2c_read(0x15, 0x02, data_raw, 7) != 0)
        return 0;

    int event = data_raw[1] >> 6; // 0=Down, 1=Up, 2=Contact

    // Finger currently touching the screen?
    bool touching = (event == 0 || event == 2);

    if (touching)
    {
        // Coordinates are valid on Down and Contact
        *x = (int)data_raw[2] + (int)(data_raw[1] & 0x0F) * 256;
        *y = (int)data_raw[4] + (int)(data_raw[3] & 0x0F) * 256;

        // Only report the first touch (rising edge): either true Down, or Contact following idle
        if (!s_touch_active)
        {
            s_touch_active = true;
            return 1; // first touch event
        }
        return 0; // still holding; do not retrigger
    }

    // Not touching anymore -> clear latch on Up
    if (event == 1)
    {
        s_touch_active = false;
    }
    return 0;
}

int i2c_read(uint16_t addr, uint8_t reg_addr, uint8_t *reg_data, uint32_t length)
{
    Wire.beginTransmission(addr);
    Wire.write(reg_addr);
    if (Wire.endTransmission(true))
        return -1;
    Wire.requestFrom((uint16_t)addr, (uint8_t)length, (bool)true);
    for (uint32_t i = 0; i < length && Wire.available(); i++)
    {
        *reg_data++ = Wire.read();
    }
    return 0;
}

int i2c_write(uint8_t addr, uint8_t reg_addr, const uint8_t *reg_data, uint32_t length)
{
    Wire.beginTransmission(addr);
    Wire.write(reg_addr);
    for (uint32_t i = 0; i < length; i++)
    {
        Wire.write(*reg_data++);
    }
    if (Wire.endTransmission(true))
        return -1;
    return 0;
}

