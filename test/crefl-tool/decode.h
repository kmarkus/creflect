/* records in decode*.bin are ASCII, values read the same on any endianness */
enum m { M = 0x35353535 };

struct rec {
	char tag[4];
	uint8_t a;
	int8_t b;
	uint16_t c;
	enum m mode;
};
