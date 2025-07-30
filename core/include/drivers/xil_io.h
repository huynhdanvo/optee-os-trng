#include <io.h>

#define Xil_Out32(addr, val)	io_write32((vaddr_t)addr, val)
#define Xil_In32(addr)	io_read32((vaddr_t)addr)
