#include "TCA9554PWR.h"

uint8_t I2C_Read_EXIO(uint8_t REG)
{
  uint8_t bitsStatus = 0;
  if (!I2C_Read(TCA9554_ADDRESS, REG, &bitsStatus, 1)) {
    printf("TCA9554 read failed\r\n");
    return 0;
  }
  return bitsStatus;
}

uint8_t I2C_Write_EXIO(uint8_t REG, uint8_t Data)
{
  if (!I2C_Write(TCA9554_ADDRESS, REG, &Data, 1)) {
    printf("TCA9554 write failed\r\n");
    return -1;
  }
  return 0;
}

void Mode_EXIO(uint8_t Pin, uint8_t State)
{
  uint8_t bitsStatus = I2C_Read_EXIO(TCA9554_CONFIG_REG);
  uint8_t Data = (0x01 << (Pin - 1)) | bitsStatus;
  I2C_Write_EXIO(TCA9554_CONFIG_REG, Data);
}

void Mode_EXIOS(uint8_t PinState)
{
  I2C_Write_EXIO(TCA9554_CONFIG_REG, PinState);
}

uint8_t Read_EXIO(uint8_t Pin)
{
  uint8_t inputBits = I2C_Read_EXIO(TCA9554_INPUT_REG);
  uint8_t bitStatus = (inputBits >> (Pin - 1)) & 0x01;
  return bitStatus;
}

uint8_t Read_EXIOS(uint8_t REG)
{
  uint8_t inputBits = I2C_Read_EXIO(REG);
  return inputBits;
}

void Set_EXIO(uint8_t Pin, uint8_t State)
{
  uint8_t Data;
  if (State < 2 && Pin < 9 && Pin > 0) {
    uint8_t bitsStatus = Read_EXIOS(TCA9554_OUTPUT_REG);
    if (State == 1)
      Data = (0x01 << (Pin - 1)) | bitsStatus;
    else
      Data = (~(0x01 << (Pin - 1))) & bitsStatus;
    I2C_Write_EXIO(TCA9554_OUTPUT_REG, Data);
  }
}

void Set_EXIOS(uint8_t PinState)
{
  I2C_Write_EXIO(TCA9554_OUTPUT_REG, PinState);
}

void Set_Toggle(uint8_t Pin)
{
  uint8_t bitsStatus = Read_EXIO(Pin);
  Set_EXIO(Pin, (bool)!bitsStatus);
}

void TCA9554PWR_Init(uint8_t PinState)
{
  Mode_EXIOS(PinState);
}
