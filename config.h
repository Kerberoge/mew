static int top              = 1;
static int case_insensitive = 1;
static int item_padding     = 20;
static const char *fonts[]  = { "DejaVu Sans Mono:size=10.5" };
static const char *prompt   = NULL;
static uint32_t colors[][2] = {
	/*               fg          bg          */
	[SchemeNorm] = { 0xddddddff, 0x1c1e1eff },
	[SchemeSel]  = { 0x65ad89ff, 0x1c1e1eff },
};

/* If nonzero, use vertical list with given number of lines */
static unsigned int lines   = 0;
