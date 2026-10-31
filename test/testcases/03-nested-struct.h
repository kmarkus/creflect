#ifndef _03_NESTED_STRUCT_H
#define _03_NESTED_STRUCT_H

#include "01-struct-basic.h"

struct period {
	struct date start;
	struct date end;
};

#endif
