static int top              = 1;
static int case_insensitive = 1;
static int item_padding     = 20;
static const char *fonts[]  = { "Open Sans SemiBold:size=9" };
static const char *prompt   = NULL;
static uint32_t colors[][2] = {
	/*               fg          bg          */
	[SchemeNorm] = { 0xeeeeeeff, 0x1d293dff },
	[SchemeSel]  = { 0x7da5ffff, 0x1d293dff },
};

/* If nonzero, use vertical list with given number of lines */
static unsigned int lines   = 0;
