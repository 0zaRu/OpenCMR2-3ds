#ifndef OPENCMR2_TEST_HIGH_ADDRESS_H
#define OPENCMR2_TEST_HIGH_ADDRESS_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *Test_AllocateHighAddress(size_t size);
void Test_FreeHighAddress(void *memory);
#ifdef __cplusplus
}
#endif
#endif
