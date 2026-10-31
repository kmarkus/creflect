typedef void (*cb_t)(int, void *);

struct ops {
	char id;
	int (*open)(const char *name, int flags);
	cb_t cb;
	void (*handlers[3])(void);
};

/* declarations that are skipped */
int some_function(int a, struct ops *o);
extern int some_var;
static inline int inline_fn(int x) { return x * 2; }
