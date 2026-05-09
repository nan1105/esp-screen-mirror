#include "ft6336u_driver.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#define TAG "FT6336U"

static i2c_master_bus_handle_t fu6336u_bus_handle = NULL;
static i2c_master_dev_handle_t fu6336u_dev_handle = NULL;
static bool ft6336u_int_flag = false;
esp_err_t ft6336u_driver(gpio_num_t sda, gpio_num_t scl){


    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .i2c_port = -1,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_new_master_bus(&bus_config,&fu6336u_bus_handle);


    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x38,
        .scl_speed_hz = 400*1000,
    };

    i2c_master_bus_add_device(fu6336u_bus_handle,&dev_config,&fu6336u_dev_handle);

    uint8_t read_buf[1];
    uint8_t write_buf[2];
    esp_err_t ret = ESP_FAIL;
    write_buf[0] = 0xA3; //register address
    ret = i2c_master_transmit_receive(fu6336u_dev_handle,&write_buf[0],1,&read_buf[0],1,1000);
    if(ret != ESP_OK){
        ESP_LOGE("FU6336U","I2C transmit receive failed");
        return ret;
    }
    write_buf[0] = 0xA4; //register address
    write_buf[1] = 0x00; //data to write back
    ret = i2c_master_transmit(fu6336u_dev_handle,&write_buf[0],2,1000);

    return ret;

}

void ft6336u_read(int16_t *x, int16_t *y,int *state){

    uint8_t read_buf[5];
    uint8_t write_buf[1];
    static int16_t last_x = 0;
    static int16_t last_y = 0;
    esp_err_t ret = ESP_FAIL;

    if(!ft6336u_int_flag){
        *x = last_x;
        *y = last_y;
        *state = 0;
        return;
    }
    write_buf[0] = 0x02; //register address
    ret = i2c_master_transmit_receive(fu6336u_dev_handle,&write_buf[0],1,&read_buf[0],5,5000);
    if(ret != ESP_OK){
        ESP_LOGE("FU6336U","I2C transmit receive failed");
        return;
    }
    if((read_buf[0] & 0x0f) != 1){
        
        *x = last_x;
        *y = last_y;
        *state = 0;
        return;
    }

    *x = ((read_buf[1] & 0x0F) << 8) | read_buf[2];
    *y = ((read_buf[3] & 0x0F) << 8) | read_buf[4];
    last_x = *x;
    last_y = *y;
    *state = 1;
    
}


void ft6336u_int_info(bool flag){
    ft6336u_int_flag = flag;

}

