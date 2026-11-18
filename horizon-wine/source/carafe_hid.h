#ifndef CARAFE_HID_H
#define CARAFE_HID_H

void* carafeHidOpen(const unsigned short* name, unsigned int bytes);
void carafeHidClose(void* file);
unsigned int carafeHidIoctl(void* file, unsigned int code, const unsigned char* in, unsigned int in_size,
    unsigned char* out, unsigned int out_max, unsigned int* out_size);
unsigned int carafeHidRead(void* file, unsigned char* out, unsigned int out_max, unsigned int* out_size);
unsigned int carafeHidWrite(void* file, const unsigned char* in, unsigned int size, unsigned int* written);
void carafeHidRegister(void);
void carafeHidPoll(void);

#endif
