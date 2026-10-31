#include <stdbool.h>

enum mode { MODE_OFF, MODE_ON, MODE_AUTO, MODE_MAX = 7 };
enum sign { SIGN_NEG = -2, SIGN_POS = 1 };

struct bf_enum_bool {
	bool on : 1;
	_Bool flag : 1;
	enum mode m : 3;
	enum sign s : 3;
	bool plain;
	char c : 5;
	signed char sc : 5;
};
